// remote_log.cpp — the log, sent to bo2-logd on the developer's PC (tools/bo2-logd), over Tailscale.
//
// Testing away from the PC means no adb: every run sends its log as it goes, so a session can be read on the PC
// while it is still being played. BO2_LOGD=host:port turns it on (Bo2App sets it from the build's bo2.logServer
// Gradle property); unset or "0", nothing is sent and nothing else changes.
//
// Protocol (tools/bo2-logd/README.md): POST /v1/ingest with X-Bo2-Session, X-Bo2-Kind (hello | log | prof) and
// X-Bo2-Seq; the body is text lines. One batch every 2 s per kind ([PROF] lines go to `prof`, the rest to `log`).
// A failed batch is retried first, with the same sequence number, so a retry of a batch the server did store is
// dropped there. The queue is bounded (2 MB): past it the oldest bytes go and the next batch says how many.
// Never blocks the caller: enqueue is a copy under a mutex; the network is the sender thread's.
#include "remote_log.hpp"

#include <android/log.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace bo2remote {

namespace {

constexpr size_t kMaxPending = 2u << 20;
constexpr size_t kMaxBatch = 512u << 10;

struct Batch {
    const char* kind;
    uint64_t seq;
    std::string body;
};

// Shared with the detached sender thread, so never destroyed: at exit a condition variable's destructor waits for
// its waiters (glibc), and the sender is always waiting on it; a static object would hang the exit.
struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    std::string log, prof;     // lines not yet in a batch
    uint64_t dropped = 0;      // bytes dropped because the queue was full
    bool on = false;
    std::string host, port;    // set once, before the sender starts
};
Shared& sh() {
    static Shared* s = new Shared;
    return *s;
}

std::string prop(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(name, v);
    return v;
}

std::string clean(std::string s) {
    for (char& c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            c = '-';
    return s;
}

// One HTTP/1.1 POST on a fresh connection; true on a 2xx answer.
bool post(const std::string& session, const char* kind, uint64_t seq, const std::string& body) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(sh().host.c_str(), sh().port.c_str(), &hints, &res) != 0 || !res) return false;
    bool ok = false;
    for (addrinfo* a = res; a && !ok; a = a->ai_next) {
        const int fd = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 3000) == 1) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                r = err == 0 ? 0 : -1;
            }
        }
        if (r == 0) {
            fcntl(fd, F_SETFL, flags);
            timeval tv{5, 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            char head[512];
            const int hn = snprintf(head, sizeof head,
                                    "POST /v1/ingest HTTP/1.1\r\nHost: %s:%s\r\nX-Bo2-Session: %s\r\nX-Bo2-Kind: %s\r\n"
                                    "X-Bo2-Seq: %llu\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\n"
                                    "Connection: close\r\n\r\n",
                                    sh().host.c_str(), sh().port.c_str(), session.c_str(), kind, (unsigned long long)seq,
                                    body.size());
            std::string out(head, hn > 0 ? (size_t)hn : 0);
            out += body;
            size_t sent = 0;
            while (sent < out.size()) {
                const ssize_t w = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
                if (w <= 0) break;
                sent += (size_t)w;
            }
            if (sent == out.size()) {
                char reply[32] = {};
                const ssize_t got = recv(fd, reply, sizeof reply - 1, 0);
                ok = got >= 12 && strncmp(reply, "HTTP/1.", 7) == 0 && reply[9] == '2';
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

// Up to kMaxBatch bytes of whole lines from the front of `pending`.
std::string take_lines(std::string& pending) {
    if (pending.size() <= kMaxBatch) return std::exchange(pending, std::string());
    size_t cut = pending.rfind('\n', kMaxBatch);
    cut = cut == std::string::npos ? kMaxBatch : cut + 1;
    std::string head = pending.substr(0, cut);
    pending.erase(0, cut);
    return head;
}

void sender_loop() {
    pthread_setname_np(pthread_self(), "bo2-logd");
    std::deque<Batch> retry;
    uint64_t seq = 0;
    int backoff_ms = 1000;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(sh().mu);
            sh().cv.wait_for(lk, std::chrono::seconds(2));
            if (sh().dropped) {
                char note[96];
                snprintf(note, sizeof note, "[LOGD] %llu bytes dropped: the PC was out of reach and the queue full\n",
                         (unsigned long long)sh().dropped);
                sh().log.insert(0, note);
                sh().dropped = 0;
            }
            if (retry.size() < 8) {
                if (!sh().log.empty()) retry.push_back({"log", ++seq, take_lines(sh().log)});
                if (!sh().prof.empty()) retry.push_back({"prof", ++seq, take_lines(sh().prof)});
            }
        }
        while (!retry.empty()) {
            if (!post(session(), retry.front().kind, retry.front().seq, retry.front().body)) break;
            retry.pop_front();
        }
        if (retry.empty()) {
            backoff_ms = 1000;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, 30000);
        }
    }
}

// The previous run's session id (its log starts with "session <id>") and the last lines of that log.
void send_previous_tail(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    char first[256] = {};
    std::string prev;
    for (int i = 0; i < 4 && fgets(first, sizeof first, f); i++) {
        const char* s = strstr(first, "session ");
        if (s) {
            prev = s + 8;
            while (!prev.empty() && (prev.back() == '\n' || prev.back() == '\r')) prev.pop_back();
            break;
        }
    }
    if (prev.empty()) { fclose(f); return; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    const long from = std::max(0L, size - (long)(96u << 10));
    fseek(f, from, SEEK_SET);
    std::string tail((size_t)(size - from), '\0');
    const size_t got = fread(tail.data(), 1, tail.size(), f);
    fclose(f);
    tail.resize(got);
    if (from > 0) {
        const size_t nl = tail.find('\n');
        if (nl != std::string::npos) tail.erase(0, nl + 1);
    }
    tail.insert(0, "[LOGD] the end of this run's log, sent by the next launch (a crash report written after the last "
                   "batch arrives this way)\n");
    if (!post(clean(prev), "log", 1000000000ull, tail))
        __android_log_print(ANDROID_LOG_INFO, "BO2", "[LOGD] previous run's tail not sent");
}

}  // namespace

const std::string& session() {
    static const std::string id = [] {
        char when[32];
        const time_t t = time(nullptr);
        tm local{};
        localtime_r(&t, &local);
        strftime(when, sizeof when, "%Y%m%d-%H%M%S", &local);
        return clean(prop("ro.product.model") + "-" + when + "-" + std::to_string(getpid()));
    }();
    return id;
}

namespace {
std::string g_previous;                // the previous run's log, for its tail; set once by start()
std::atomic<bool> g_launched{false};   // the sender thread runs (it is never stopped, only idle while off)

void launch() {
    if (g_launched.exchange(true)) return;
    const std::string previous = g_previous;
    std::thread([previous] {
        pthread_setname_np(pthread_self(), "bo2-logd");
        const char* build = getenv("BO2_BUILD");
        std::string hello = "session=" + session() + "\ndevice=" + prop("ro.product.model") +
                            "\nandroid=" + prop("ro.build.version.release") + " (API " + prop("ro.build.version.sdk") +
                            ")\nsoc=" + prop("ro.soc.model") + "\nbuild=" + (build ? build : "?") + "\n";
        if (const char* remote = getenv("BO2_REMOTE_ENV")) hello += std::string("remote_env=") + remote + "\n";
        for (int attempt = 0; attempt < 5 && !post(session(), "hello", 0, hello); attempt++)
            std::this_thread::sleep_for(std::chrono::seconds(2));
        send_previous_tail(previous);
        sender_loop();
    }).detach();
    __android_log_print(ANDROID_LOG_INFO, "BO2", "[LOGD] sending this run's log to %s:%s as session %s",
                        sh().host.c_str(), sh().port.c_str(), session().c_str());
}
}  // namespace

void start(const std::string& log_dir, const char* previous_log_name) {
    const char* e = getenv("BO2_LOGD");
    if (!e || !*e || strcmp(e, "0") == 0) return;
    const std::string target(e);
    const size_t colon = target.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        __android_log_print(ANDROID_LOG_WARN, "BO2", "[LOGD] BO2_LOGD=%s is not host:port: off", e);
        return;
    }
    sh().host = target.substr(0, colon);
    sh().port = target.substr(colon + 1);
    g_previous = log_dir + "/" + previous_log_name;
    const char* diag = getenv("BO2_DIAG");
    if (diag && atoi(diag) >= 2) {
        __android_log_print(ANDROID_LOG_INFO, "BO2", "[LOGD] not sending (Logging: Minimal)");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(sh().mu);
        sh().on = true;
    }
    launch();
}

void set_enabled(bool on) {
    if (sh().host.empty()) return;   // no log server in this build, or start() not run yet
    {
        std::lock_guard<std::mutex> lk(sh().mu);
        if (sh().on == on) return;
        sh().on = on;
        if (!on) {
            sh().log.clear();
            sh().prof.clear();
        }
    }
    if (on) launch();
}

void enqueue(const char* text, size_t n) {
    if (!n) return;
    std::lock_guard<std::mutex> lk(sh().mu);
    if (!sh().on) return;
    // Lines carry the log file's "%9.3f " time first; the tag follows it.
    const char* tag = static_cast<const char*>(memchr(text, '[', std::min<size_t>(n, 16)));
    const bool prof = tag && static_cast<size_t>(text + n - tag) > 6 && strncmp(tag, "[PROF]", 6) == 0;
    std::string& to = prof ? sh().prof : sh().log;
    to.append(text, n);
    if (sh().log.size() + sh().prof.size() > kMaxPending) {
        std::string& big = sh().log.size() >= sh().prof.size() ? sh().log : sh().prof;
        size_t cut = big.find('\n', big.size() / 2);
        cut = cut == std::string::npos ? big.size() / 2 : cut + 1;
        sh().dropped += cut;
        big.erase(0, cut);
    }
    if (sh().log.size() + sh().prof.size() > kMaxBatch) sh().cv.notify_one();
}

}  // namespace bo2remote
