// bo2_platform_android.cpp — what bo2_platform.h declares for Android (bionic arm64). Not built on macOS.
#if defined(__ANDROID__) || defined(__linux__)
#include "bo2_platform.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <unistd.h>

// ---- libdispatch semaphores -------------------------------------------------------------------------------------
static uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

extern "C" dispatch_semaphore_t bo2_dispatch_semaphore_create(long value) {
    if (value < 0) return nullptr;   // as libdispatch
    auto* s = new bo2_dispatch_semaphore;
    if (sem_init(&s->sem, 0, (unsigned)value) != 0) { delete s; return nullptr; }
    return s;
}

// libdispatch returns non-zero when a thread was woken. Nobody in the runtime reads it; 0 is returned.
extern "C" long bo2_dispatch_semaphore_signal(dispatch_semaphore_t s) {
    if (s) sem_post(&s->sem);
    return 0;
}

// dispatch_time(DISPATCH_TIME_NOW, delta): an absolute CLOCK_MONOTONIC deadline in ns.
extern "C" dispatch_time_t bo2_dispatch_time(dispatch_time_t when, int64_t delta_ns) {
    if (when == DISPATCH_TIME_FOREVER) return DISPATCH_TIME_FOREVER;
    const uint64_t base = when == DISPATCH_TIME_NOW ? mono_ns() : when;
    if (delta_ns < 0 && (uint64_t)(-delta_ns) > base) return 1;
    return base + (uint64_t)delta_ns;
}

// 0 when the semaphore was taken, non-zero on timeout (as libdispatch).
extern "C" long bo2_dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t deadline) {
    if (!s) return -1;
    if (deadline == DISPATCH_TIME_FOREVER) {
        while (sem_wait(&s->sem) != 0)
            if (errno != EINTR) return -1;
        return 0;
    }
    const timespec ts = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (sem_clockwait(&s->sem, CLOCK_MONOTONIC, &ts) != 0) {
        if (errno == EINTR) continue;
        return errno == ETIMEDOUT ? 1 : -1;
    }
    return 0;
}

// ---- QoS ----------------------------------------------------------------------------------------------------------
static thread_local qos_class_t t_qos = QOS_CLASS_DEFAULT;

extern "C" int pthread_set_qos_class_self_np(qos_class_t qos, int) {
    // Android's own scale: THREAD_PRIORITY_URGENT_DISPLAY is -8, THREAD_PRIORITY_DEFAULT 0, BACKGROUND 10.
    int nice = 0;
    if (qos >= QOS_CLASS_USER_INTERACTIVE) nice = -8;
    else if (qos >= QOS_CLASS_USER_INITIATED) nice = -4;
    else if (qos <= QOS_CLASS_BACKGROUND && qos != QOS_CLASS_UNSPECIFIED) nice = 10;
    if (setpriority(PRIO_PROCESS, (id_t)gettid(), nice) != 0) return errno;
    t_qos = qos;
    return 0;
}

extern "C" qos_class_t qos_class_self(void) { return t_qos; }

// ---- First entry of a coroutine onto a fresh stack ----------------------------------------------------------------
// bionic's ucontext_t is the kernel's: uc_mcontext.regs[31], sp, pc. makecontext records the entry function in
// x19 and the link in x20, as glibc's arm64 makecontext does; setcontext of such a context switches to its stack and
// calls the function with a zeroed frame chain (fp = lr = 0), so unwinders stop there. x18 (the shadow call stack
// pointer on Android) is left alone. Contexts from getcontext alone are never resumed by the runtime: refused.
static constexpr uint64_t kMadeContext = 0x62326d6b63747831ull;   // "b2mkctx1", in uc_mcontext.regs[0]

extern "C" __attribute__((noreturn, used)) void bo2_context_returned(const ucontext_t* link) {
    fprintf(stderr, "[CTX] a coroutine entry function returned (uc_link=%p): Darwin would end the thread here; "
                    "the runtime never expects it. Aborting.\n", (const void*)link);
    abort();
}

// bo2_context_jump(sp, fn, link): never returns.
extern "C" __attribute__((noreturn)) void bo2_context_jump(uint64_t sp, void (*fn)(), const ucontext_t* link);
__asm__(
    ".text\n"
    ".balign 16\n"
    ".type bo2_context_jump, %function\n"
    "bo2_context_jump:\n"
    "    bti c\n"
    "    mov sp, x0\n"
    "    mov x19, x1\n"
    "    mov x20, x2\n"
    "    mov x29, xzr\n"
    "    mov x30, xzr\n"
    "    blr x19\n"
    "    mov x0, x20\n"
    "    bl bo2_context_returned\n"
    "    brk #0x3e8\n"
    ".size bo2_context_jump, .-bo2_context_jump\n");

extern "C" int bo2_getcontext(ucontext_t* uc) {
    if (!uc) { errno = EINVAL; return -1; }
    memset(&uc->uc_mcontext, 0, sizeof uc->uc_mcontext);
    uc->uc_link = nullptr;
    uc->uc_stack.ss_sp = nullptr;
    uc->uc_stack.ss_size = 0;
    uc->uc_stack.ss_flags = 0;
    return 0;
}

extern "C" void bo2_makecontext(ucontext_t* uc, void (*fn)(), int argc, ...) {
    if (!uc || !fn || argc != 0 || !uc->uc_stack.ss_sp || uc->uc_stack.ss_size < 4096) {
        fprintf(stderr, "[CTX] makecontext: unsupported arguments (fn=%p argc=%d stack=%p+%zu); only fn() on a "
                        "stack of 4 KiB or more is implemented\n", (void*)fn, argc, uc ? uc->uc_stack.ss_sp : nullptr,
                uc ? (size_t)uc->uc_stack.ss_size : (size_t)0);
        abort();
    }
    const uint64_t top = ((uint64_t)(uintptr_t)uc->uc_stack.ss_sp + uc->uc_stack.ss_size) & ~15ull;
    uc->uc_mcontext.regs[0] = kMadeContext;
    uc->uc_mcontext.regs[19] = (uint64_t)(uintptr_t)fn;
    uc->uc_mcontext.regs[20] = (uint64_t)(uintptr_t)uc->uc_link;
    uc->uc_mcontext.sp = top;
    uc->uc_mcontext.pc = (uint64_t)(uintptr_t)&bo2_context_jump;
}

extern "C" int bo2_setcontext(const ucontext_t* uc) {
    if (!uc || uc->uc_mcontext.regs[0] != kMadeContext) {
        fprintf(stderr, "[CTX] setcontext on a context makecontext did not prepare: not implemented on Android\n");
        abort();
    }
    bo2_context_jump(uc->uc_mcontext.sp, (void (*)())(uintptr_t)uc->uc_mcontext.regs[19],
                     (const ucontext_t*)(uintptr_t)uc->uc_mcontext.regs[20]);
}

// ---- Resident size ------------------------------------------------------------------------------------------------
extern "C" uint64_t bo2_host_rss_mb(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size = 0, resident = 0;
    const int n = fscanf(f, "%llu %llu", &size, &resident);
    fclose(f);
    if (n != 2) return 0;
    return (uint64_t)resident * (uint64_t)sysconf(_SC_PAGESIZE) >> 20;
}

// ---- Triple DES (FIPS 46-3 / SP 800-67), for the Demonware responder ---------------------------------------------
// Straight from the standard's tables; bits are numbered 1..n from the most significant, as in the standard.
namespace {
const uint8_t kIP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22,
                         14, 6,  64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9,  1,  59, 51, 43, 35,
                         27, 19, 11, 3,  61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
const uint8_t kFP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22,
                         62, 30, 37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11,
                         51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25};
const uint8_t kE[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
                        16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
const uint8_t kP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                        2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
const uint8_t kPC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                          35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                          46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
const uint8_t kPC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                          26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                          51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
const uint8_t kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
const uint8_t kS[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7, 0, 15, 7,  4,  14, 2,  13, 1,  10, 6, 12, 11,
     9,  5,  3,  8, 4,  1,  14, 8,  13, 6,  2,  11, 15, 12, 9, 7, 3, 10, 5,  0,  15, 12, 8,  2,  4,  9, 1,  7,
     5,  11, 3,  14, 10, 0, 6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3,  13, 4, 7,  15, 2, 8,  14, 12, 0,
     1,  10, 6,  9,  11, 5,  0,  14, 7,  11, 10, 4, 13, 1, 5,  8,  12, 6,  9, 3,  2,  15, 13, 8, 10, 1,
     3,  15, 4,  2,  11, 6,  7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,
     6,  10, 2,  8,  5,  14, 12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12,
     5,  10, 14, 7,  1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3, 0, 6,  9,  10, 1, 2,  8, 5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15,
     0,  3,  4,  7, 2, 12, 1,  10, 14, 9, 10, 6, 9,  0,  12, 11, 7,  13, 15, 1,  3,  14,
     5,  2,  8,  4, 3, 15, 0,  6,  10, 1, 13, 8, 9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0,  14, 9,  14, 11, 2,  12, 4,  7,
     13, 1,  5,  0,  15, 10, 3,  9,  8,  6,  4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
     6,  3,  0,  14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9, 2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12,
     9,  5,  6,  1,  13, 14, 0,  11, 3,  8,  9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10,
     1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0,  8,  13, 3,  12, 9,  7,  5,  10, 6,  1,  13, 0,  11, 7,  4,  9,
     1,  10, 14, 3,  5,  12, 2,  15, 8,  6,  1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,
     0,  5,  9,  2,  6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,
     7,  4,  12, 5,  6,  11, 0,  14, 9,  2,  7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13,
     15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11}};

uint64_t permute(uint64_t in, const uint8_t* table, int n, int in_bits) {
    uint64_t out = 0;
    for (int i = 0; i < n; ++i) out = (out << 1) | ((in >> (in_bits - table[i])) & 1u);
    return out;
}

struct DesKey { uint64_t sub[16]; };   // 48-bit round keys

DesKey des_schedule(const uint8_t k[8]) {
    uint64_t key = 0;
    for (int i = 0; i < 8; ++i) key = (key << 8) | k[i];
    const uint64_t cd = permute(key, kPC1, 56, 64);
    uint32_t c = (uint32_t)(cd >> 28) & 0x0fffffffu, d = (uint32_t)cd & 0x0fffffffu;
    DesKey ks;
    for (int r = 0; r < 16; ++r) {
        for (int s = 0; s < kShifts[r]; ++s) {
            c = ((c << 1) | (c >> 27)) & 0x0fffffffu;
            d = ((d << 1) | (d >> 27)) & 0x0fffffffu;
        }
        ks.sub[r] = permute(((uint64_t)c << 28) | d, kPC2, 48, 56);
    }
    return ks;
}

uint32_t feistel(uint32_t r, uint64_t k) {
    const uint64_t x = permute(r, kE, 48, 32) ^ k;
    uint32_t s = 0;
    for (int i = 0; i < 8; ++i) {
        const unsigned b = (unsigned)(x >> (42 - 6 * i)) & 0x3fu;
        s = (s << 4) | kS[i][((b & 0x20u) >> 4 | (b & 1u)) * 16 + ((b >> 1) & 0xfu)];
    }
    return (uint32_t)permute(s, kP, 32, 32);
}

uint64_t des_block(uint64_t block, const DesKey& ks, bool decrypt) {
    const uint64_t ip = permute(block, kIP, 64, 64);
    uint32_t l = (uint32_t)(ip >> 32), r = (uint32_t)ip;
    for (int i = 0; i < 16; ++i) {
        const uint32_t t = r;
        r = l ^ feistel(r, ks.sub[decrypt ? 15 - i : i]);
        l = t;
    }
    return permute(((uint64_t)r << 32) | l, kFP, 64, 64);
}

uint64_t load_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
void store_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)v; v >>= 8; }
}
}  // namespace

extern "C" bool bo2_des3_cbc(bool encrypt, const uint8_t key[24], const uint8_t iv[8], uint8_t* buf, size_t n) {
    if (n % 8 != 0) return false;
    const DesKey k1 = des_schedule(key), k2 = des_schedule(key + 8), k3 = des_schedule(key + 16);
    uint64_t chain = load_be64(iv);
    for (size_t off = 0; off < n; off += 8) {
        const uint64_t in = load_be64(buf + off);
        uint64_t out;
        if (encrypt) {
            out = des_block(des_block(des_block(in ^ chain, k1, false), k2, true), k3, false);
            chain = out;
        } else {
            out = des_block(des_block(des_block(in, k3, true), k2, false), k1, true) ^ chain;
            chain = in;
        }
        store_be64(buf + off, out);
    }
    return true;
}

#endif  // __ANDROID__ || __linux__
