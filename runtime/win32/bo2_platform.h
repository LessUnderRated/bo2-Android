// bo2_platform.h — the host primitives that differ between macOS (Darwin arm64) and Android (bionic arm64).
//
// The runtime was written on Darwin. Everything it asks of the host that bionic spells differently, or does not
// have, goes through this header, so that each call site changes by one name rather than by an #ifdef block, and
// the macOS build sees exactly the calls it made before.
//
//   thread pointer      Darwin's pthread_self() lives in tpidrro_el0; Linux user space uses tpidr_el0, and
//                       tpidrro_el0 reads 0 there (every thread would look like the same thread).
//   signal context      Darwin: uc->uc_mcontext->__ss.__pc ; Linux: uc->uc_mcontext.pc.
//   ucontext coroutines bionic declares ucontext_t but exports no getcontext/makecontext/setcontext.
//   raw monotonic clock CLOCK_UPTIME_RAW + clock_gettime_nsec_np are Darwin's.
//   dispatch semaphores libdispatch is not on Android.
//   thread names, QoS   Darwin's pthread_setname_np names only the caller; QoS classes are Darwin's.
//   3DES                CommonCrypto is Darwin's (the Demonware responder, dw_local.cpp).
#pragma once
#include <cstdint>
#include <cstddef>
#include <ctime>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>

#if defined(__APPLE__)

#include <pthread/qos.h>
#include <dispatch/dispatch.h>

static inline uint64_t bo2_read_thread_pointer() {
    uint64_t v;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v));
    return v;
}
#define BO2_UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define BO2_UC_LR(uc) ((uc)->uc_mcontext->__ss.__lr)
#define BO2_UC_FP(uc) ((uc)->uc_mcontext->__ss.__fp)
#define BO2_UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define BO2_UC_X(uc, n) ((uc)->uc_mcontext->__ss.__x[(n)])
#define BO2_UC_HAS_MCONTEXT(uc) ((uc) && (uc)->uc_mcontext)
#define BO2_ST_BIRTHTIM(st) ((st).st_birthtimespec)
#define BO2_ST_ATIM(st) ((st).st_atimespec)
#define BO2_ST_MTIM(st) ((st).st_mtimespec)
// The 8-byte TSD slots C33 reads at *(thread pointer + 8 * key): Darwin's layout only.
#define BO2_HAS_DARWIN_TSD 1
// The start of this image, below all its code: lifted functions are cached as 32-bit offsets from it (C32).
extern "C" const struct mach_header_64 _mh_execute_header;
#define BO2_IMAGE_BASE (reinterpret_cast<const char*>(&_mh_execute_header))

#elif defined(__ANDROID__) || defined(__linux__)

#include <semaphore.h>
#include <sys/resource.h>
#include <unistd.h>

static inline uint64_t bo2_read_thread_pointer() {
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));   // unique per thread, never 0 for a live thread
    return v;
}
#define BO2_UC_PC(uc) ((uc)->uc_mcontext.pc)
#define BO2_UC_LR(uc) ((uc)->uc_mcontext.regs[30])
#define BO2_UC_FP(uc) ((uc)->uc_mcontext.regs[29])
#define BO2_UC_SP(uc) ((uc)->uc_mcontext.sp)
#define BO2_UC_X(uc, n) ((uc)->uc_mcontext.regs[(n)])
#define BO2_UC_HAS_MCONTEXT(uc) ((uc) != nullptr)
// Linux keeps no birth time in struct stat; the status change time is the closest stand-in Windows code accepts.
#define BO2_ST_BIRTHTIM(st) ((st).st_ctim)
#define BO2_ST_ATIM(st) ((st).st_atim)
#define BO2_ST_MTIM(st) ((st).st_mtim)
#define BO2_HAS_DARWIN_TSD 0
// The start of this image (libmain.so), below all its code: lld defines __ehdr_start for every ELF module.
extern "C" const char __ehdr_start[] __attribute__((visibility("hidden")));
#define BO2_IMAGE_BASE (__ehdr_start)

// CLOCK_UPTIME_RAW: monotonic, not slewed by NTP, stopped during sleep. CLOCK_MONOTONIC_RAW is the same on Linux.
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC_RAW
#endif
static inline uint64_t clock_gettime_nsec_np(clockid_t c) {
    timespec ts;
    if (clock_gettime(c, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---- libdispatch semaphores, the subset the runtime uses (create, signal, wait with a deadline) -------------
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW (0ull)
#define DISPATCH_TIME_FOREVER (~0ull)
struct bo2_dispatch_semaphore { sem_t sem; };
typedef bo2_dispatch_semaphore* dispatch_semaphore_t;
extern "C" dispatch_semaphore_t bo2_dispatch_semaphore_create(long value);
extern "C" long bo2_dispatch_semaphore_signal(dispatch_semaphore_t s);
extern "C" long bo2_dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t deadline);
extern "C" dispatch_time_t bo2_dispatch_time(dispatch_time_t when, int64_t delta_ns);
#define dispatch_semaphore_create bo2_dispatch_semaphore_create
#define dispatch_semaphore_signal bo2_dispatch_semaphore_signal
#define dispatch_semaphore_wait bo2_dispatch_semaphore_wait
#define dispatch_time bo2_dispatch_time

// ---- Darwin's one-argument pthread_setname_np names the calling thread ---------------------------------------
// A C++ overload next to bionic's extern "C" two-argument one. Linux allows 15 characters plus the NUL.
static inline int pthread_setname_np(const char* name) {
    char n[16];
    size_t i = 0;
    for (; name && name[i] && i < sizeof n - 1; ++i) n[i] = name[i];
    n[i] = 0;
    return pthread_setname_np(pthread_self(), n);
}

// Darwin returns the TOP (highest address) of a thread's stack.
static inline void* pthread_get_stackaddr_np(pthread_t t) {
    pthread_attr_t a;
    void* lo = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(t, &a) != 0) return nullptr;
    pthread_attr_getstack(&a, &lo, &size);
    pthread_attr_destroy(&a);
    return (char*)lo + size;
}

// ---- QoS classes. Android has none; USER_INTERACTIVE becomes the display priority (nice -8), which apps may
// set (Android raises RLIMIT_NICE for them). The class is remembered per thread for qos_class_self(). ----------
typedef unsigned int qos_class_t;
#define QOS_CLASS_USER_INTERACTIVE 0x21u
#define QOS_CLASS_USER_INITIATED 0x19u
#define QOS_CLASS_DEFAULT 0x15u
#define QOS_CLASS_UTILITY 0x11u
#define QOS_CLASS_BACKGROUND 0x09u
#define QOS_CLASS_UNSPECIFIED 0x00u
extern "C" int pthread_set_qos_class_self_np(qos_class_t qos, int relative_priority);
extern "C" qos_class_t qos_class_self(void);

// ---- ucontext coroutines: the three calls the scheduler makes, on bionic's own ucontext_t -------------------
// Only what kernel32_impl.cpp needs: getcontext to initialise, makecontext(ctx, fn, 0) on a fresh stack with
// uc_link == nullptr, and setcontext for the first entry. Later switches use _setjmp/_longjmp (C19). fn must not
// return: with uc_link null, Darwin ends the thread; here the process aborts with a message.
extern "C" int bo2_getcontext(ucontext_t* uc);
extern "C" void bo2_makecontext(ucontext_t* uc, void (*fn)(), int argc, ...);
extern "C" int bo2_setcontext(const ucontext_t* uc);
#define getcontext bo2_getcontext
#define makecontext bo2_makecontext
#define setcontext bo2_setcontext

// Resident size of the whole process, in MiB (0 when unknown): /proc/self/statm, for Darwin's task_info.
extern "C" uint64_t bo2_host_rss_mb(void);
// Triple DES (EDE, 24-byte key) in CBC mode without padding, in place; false if the size is not a multiple of 8.
// Darwin uses CommonCrypto's CCCrypt for the same operation.
extern "C" bool bo2_des3_cbc(bool encrypt, const uint8_t key[24], const uint8_t iv[8], uint8_t* buf, size_t n);

#else
#error "bo2_platform.h: only Darwin arm64 and Android/Linux arm64 are supported"
#endif

