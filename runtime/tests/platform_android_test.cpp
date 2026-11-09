// Tests for bo2-mac's Android platform layer (patches/bo2-mac: bo2_platform.h, bo2_platform_android.cpp).
// Built as a static arm64 executable with the NDK and run under qemu-aarch64 (scripts/test-arm64.sh) or on a phone.
#include "bo2_platform.h"

#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    for (; s[0] && s[1]; s += 2) {
        unsigned b;
        sscanf(s, "%2x", &b);
        v.push_back((uint8_t)b);
    }
    return v;
}

// ---- DES / 3DES ---------------------------------------------------------------------------------------------------
static void test_des3() {
    // Single DES as 3DES with K1 = K2 = K3: the classic worked example (key 133457799BBCDFF1).
    {
        auto k = hex("133457799BBCDFF1133457799BBCDFF1133457799BBCDFF1");
        auto iv = hex("0000000000000000");
        auto b = hex("0123456789ABCDEF");
        CHECK(bo2_des3_cbc(true, k.data(), iv.data(), b.data(), b.size()));
        CHECK(b == hex("85E813540F0AB405"));
    }
    // SP 800-67 rev. 1, the TDEA example: three distinct keys; block 1 of ECB equals CBC with a zero IV.
    {
        auto k = hex("0123456789ABCDEF23456789ABCDEF01456789ABCDEF0123");
        auto iv = hex("0000000000000000");
        auto b = hex("5468652071756663");   // "The qufc"
        CHECK(bo2_des3_cbc(true, k.data(), iv.data(), b.data(), b.size()));
        CHECK(b == hex("A826FD8CE53B855F"));
    }
    // CBC over several blocks, checked against OpenSSL (des-ede3-cbc -nopad): scripts/test-arm64.sh prints the
    // command; the expected value is pinned here.
    {
        auto k = hex("0123456789ABCDEF23456789ABCDEF01456789ABCDEF0123");
        auto iv = hex("1122334455667788");
        auto b = hex("54686520717566636B2062726F776E20666F78206A756D70");
        CHECK(bo2_des3_cbc(true, k.data(), iv.data(), b.data(), b.size()));
        const char* want = getenv("BO2_TEST_DES3_CBC");
        if (want) CHECK(b == hex(want));
        CHECK(bo2_des3_cbc(false, k.data(), iv.data(), b.data(), b.size()));
        CHECK(b == hex("54686520717566636B2062726F776E20666F78206A756D70"));
    }
    {
        uint8_t k[24] = {}, iv[8] = {}, b[7] = {};
        CHECK(!bo2_des3_cbc(true, k, iv, b, sizeof b));
    }
}

// ---- Coroutines: first entry by makecontext/setcontext, later switches by _setjmp/_longjmp (as coop_switch) ----
namespace co {
struct Co {
    jmp_buf jb;
    bool live = false;
    ucontext_t ctx;
    char* stack = nullptr;
};
Co g_main, g_a, g_b;
std::vector<int> g_trace;

void sw(Co* from, Co* to) {
    from->live = true;
    if (_setjmp(from->jb) == 0) {
        if (to->live) _longjmp(to->jb, 1);
        setcontext(&to->ctx);
    }
}
void entry_a() {
    volatile double f = 1.5;   // callee-saved FP state must survive switches
    for (int i = 0; i < 3; ++i) {
        g_trace.push_back(10 + i);
        sw(&g_a, &g_b);
        f *= 2;
    }
    g_trace.push_back(f == 12.0 ? 19 : -19);
    sw(&g_a, &g_main);
    abort();   // never resumed
}
void entry_b() {
    char local[256];
    memset(local, 0x5a, sizeof local);
    for (int i = 0; i < 3; ++i) {
        g_trace.push_back(20 + i);
        sw(&g_b, &g_a);
    }
    g_trace.push_back(local[100] == 0x5a ? 29 : -29);
    sw(&g_b, &g_main);
    abort();
}
void make(Co& c, void (*fn)()) {
    const size_t size = 256 << 10;
    c.stack = (char*)malloc(size);
    CHECK(getcontext(&c.ctx) == 0);
    c.ctx.uc_stack.ss_sp = c.stack;
    c.ctx.uc_stack.ss_size = size;
    c.ctx.uc_link = nullptr;
    makecontext(&c.ctx, fn, 0);
}
}  // namespace co

static void test_coroutines() {
    using namespace co;
    make(g_a, entry_a);
    make(g_b, entry_b);
    sw(&g_main, &g_a);   // a runs until it hands back to main
    const std::vector<int> want = {10, 20, 11, 21, 12, 22, 19};
    CHECK(g_trace == want);
    // b is parked inside its loop; resume it and let it finish.
    sw(&g_main, &g_b);
    CHECK(g_trace.back() == 29);
    // The stack really was the coroutine's: a frame address inside the allocation.
    static uintptr_t seen;
    struct T {
        static void probe() {
            seen = (uintptr_t)__builtin_frame_address(0);
            sw(&g_a, &g_main);
        }
    };
    Co fresh;
    g_a = fresh;
    make(g_a, T::probe);
    sw(&g_main, &g_a);
    CHECK(seen > (uintptr_t)g_a.stack && seen < (uintptr_t)g_a.stack + (256 << 10));
}

// ---- Thread pointer: unique per thread, stable in a thread, never 0 ------------------------------------------
static void test_thread_pointer() {
    const uint64_t me = bo2_read_thread_pointer();
    CHECK(me != 0);
    CHECK(bo2_read_thread_pointer() == me);
    std::atomic<uint64_t> other{0};
    std::thread t([&] { other = bo2_read_thread_pointer(); });
    t.join();
    CHECK(other != 0 && other != me);
}

// ---- dispatch semaphores ------------------------------------------------------------------------------------------
static void test_semaphores() {
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    CHECK(s != nullptr);
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    CHECK(dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 20 * 1000000ll)) != 0);   // times out
    const uint64_t waited = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    CHECK(waited >= 15 * 1000000ull && waited < 2000 * 1000000ull);
    dispatch_semaphore_signal(s);
    CHECK(dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 0)) == 0);   // already signalled
    std::thread t([&] { dispatch_semaphore_signal(s); });
    CHECK(dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER) == 0);
    t.join();
    CHECK(dispatch_semaphore_create(-1) == nullptr);
}

// ---- Small things -------------------------------------------------------------------------------------------------
static void test_misc() {
    CHECK(pthread_setname_np("bo2_ordonnanceur_trop_long") == 0);   // truncated to 15, not refused
    char name[32] = {};
    pthread_getname_np(pthread_self(), name, sizeof name);
    CHECK(strcmp(name, "bo2_ordonnanceu") == 0);
    void* top = pthread_get_stackaddr_np(pthread_self());
    const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    CHECK(top != nullptr && (uintptr_t)top > here && (uintptr_t)top - here < (64u << 20));
    CHECK(qos_class_self() == QOS_CLASS_DEFAULT);
    CHECK(bo2_host_rss_mb() > 0 || getenv("BO2_TEST_NO_PROC"));
}

int main() {
    test_des3();
    test_coroutines();
    test_thread_pointer();
    test_semaphores();
    test_misc();
    if (g_failed) {
        fprintf(stderr, "platform_android_test: %d failed\n", g_failed);
        return 1;
    }
    printf("platform_android_test: all passed\n");
    return 0;
}
