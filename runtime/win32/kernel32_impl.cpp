// kernel32_impl.cpp — real POSIX implementations of KERNEL32 imports.
//
// Each function reads args from the x86 stack, does the work via POSIX/macOS,
// writes the return to EAX, and adjusts ESP for stdcall cleanup.
//
// Implemented (real POSIX):
//   Heap: GetProcessHeap, HeapAlloc, HeapFree, HeapCreate, HeapDestroy, HeapSize
//   Thread: GetCurrentThreadId, GetCurrentProcessId, GetCurrentThread, GetCurrentProcess
//   Time: GetTickCount, QueryPerformanceCounter, QueryPerformanceFrequency
//   Memory: VirtualAlloc, VirtualFree
//   File: CreateFileA, ReadFile, WriteFile, CloseHandle, GetFileSize, SetFilePointer
//   Sync: InitializeCriticalSection, DeleteCriticalSection, EnterCriticalSection,
//         LeaveCriticalSection, TryEnterCriticalSection
//   Error: GetLastError, SetLastError
//   String: lstrlenA, lstrlenW, lstrcmpiW
//   Misc: IsDebuggerPresent, GetCommandLineA, OutputDebugStringA, MulDiv, ExitProcess
//
// Stubbed (return 0 or fake handle — to be implemented as needed):
//   CreateThread, Sleep, CreateEventA, SetEvent, WaitForSingleObject, etc.
#define _XOPEN_SOURCE 700          // macOS: enable the ucontext coroutine routines
#define _DARWIN_C_SOURCE           // ...while keeping the Darwin malloc/extensions
#include "win32_shim.h"
#include "bo2_getenv.h"
#include "bo2_hot.h"
#include "diag_counter.h"      // P660/C14 : compteurs de diagnostic sans verrou
#include <cstdint>
#include <execinfo.h>
#include <dlfcn.h>
#include <cstring>
#include <csetjmp>
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <ucontext.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include "bo2_platform.h"       // QoS, semaphores (C25), contextes, pointeur de fil : Darwin ou Android
#include <time.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <mach/mach.h>          // P795 : [FILS-CPU], temps CPU par fil
#include <mach/mach_vm.h>       // Z-NATIF-23b : [A-COUP-FIL], pile hote d'un autre fil
#include <mach/mach_time.h>
#include <libproc.h>
#endif
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <string>
extern "C" bool bo2_regles_situe(uint32_t);

// --- memory access (provided by the runtime) ---
extern "C" {
    uint32_t win32_mem_read32(uint32_t a);
    void win32_mem_write32(uint32_t a, uint32_t v);
    uint8_t win32_mem_read8(uint32_t a);
    void win32_mem_write8(uint32_t a, uint8_t v);
    uint32_t __remill_symbolic_FSBASE(void);
}

// --- guest-side allocator -------------------------------------------------
// The guest is 32-bit; a host pointer (malloc/mmap) is 64-bit and truncates
// when stored in a 32-bit guest slot or returned in EAX. So every allocator /
// buffer-returning shim hands out GUEST addresses from a reserved arena and
// lets the guest read/write through g_mem — no host pointer ever enters guest
// memory. g_mem is sparse, so unwritten bytes read as 0 (HEAP_ZERO_MEMORY and
// null-termination come for free).
//
// Guest map: stack 0x00100000-0x00110000, PE image 0x00400000+, cmdline
// scratch 0x0F000000, TEB/PEB 0x7FFD0000+. The arena is disjoint from all.
// ARENA SIZE. 0x20000000..0x70000000 (1.25 GiB) was enough for the frontend and
// is NOT enough for a campaign level: afghanistan exhausts it — 1275 MiB handed
// out of 1280, on a 10 MB request, with 528 MiB in one live block, 403 MiB in
// 305 blocks and 177 MiB in 16. Those are legitimate engine allocations the
// menus never make, not a leak (7,896 load-complete cycles produced 305 blocks,
// not thousands), and our bump allocator reuses far worse than a real heap does,
// so it needs headroom rather than exactly the game's own footprint.
//
// Growing it is nearly free: win32_runtime.cpp maps the WHOLE 4 GiB guest space
// once with MAP_NORESERVE, so untouched arena pages cost nothing. And it is safe:
// t6sp.exe sets IMAGE_FILE_LARGE_ADDRESS_AWARE (COFF characteristics 0x0123), so
// the guest is declared correct with pointers that have bit 31 set — which is
// also why lpMaximumApplicationAddress had to be corrected to the WOW64 value
// (see GetSystemInfo below; it was reporting the 2 GiB non-large-address one).
//
// The one thing in the way is the TEB/PEB block at 0x7FFD0000 (PEB 0x7FFD4000,
// main TLS 0x7FFD8000), so the arena is two segments with that region punched
// out rather than one range: 0x20000000..0x7FF00000 then 0x80000000..0xF0000000,
// which is 1.5 + 1.75 = 3.25 GiB.
static constexpr uint32_t GUEST_HEAP_BASE = 0x20000000u;
static constexpr uint32_t GUEST_HEAP_END  = 0xF0000000u;
static constexpr uint32_t GUEST_TEB_HOLE_BASE = 0x7FF00000u;   // TEB/PEB/TLS live here
static constexpr uint32_t GUEST_TEB_HOLE_END  = 0x80000000u;
static uint32_t g_heap_next = GUEST_HEAP_BASE;
// guest address handed out -> byte size, for HeapReAlloc / HeapSize.
static std::unordered_map<uint32_t, uint32_t> g_alloc_sizes;

// --- FREE-BLOCK REUSE --------------------------------------------------------
//
// "Never frees" was fine while the guest only did coarse, mostly one-shot
// allocations. IT STOPPED BEING FINE THE MOMENT THE VIDEO DECODED: libvpx
// allocates and releases frame buffers per frame (1280x720 YUV is over a
// megabyte each), so a bump allocator burns the whole 1.25 GiB arena in a couple
// of minutes and the game then puts up its own
//     "Out of memory error. You are probably low on hard disk space."
// and calls ExitProcess. That is the guest reporting OUR bug accurately.
//
// So HeapFree/VirtualFree now return the block to a size-bucketed free list and
// guest_alloc reuses one when the fit is good. Deliberately conservative:
//  * NO COALESCING and no splitting — blocks keep the exact extent they were
//    handed out with, so a reused address can never overlap a live neighbour.
//  * a block is reused only when it is at least as big as the request and not
//    more than twice as big, which bounds internal waste and keeps the big
//    video buffers from being eaten by small requests.
//  * alignment is re-checked; a block that does not satisfy the requested
//    alignment is simply skipped rather than adjusted.
// Handing an address out twice is what Windows does too, so a guest
// use-after-free now behaves as it would there — which is the point of a shim.
// BO2_NO_HEAP_REUSE=1 restores the old bump-only behaviour for bisecting.
static std::unordered_map<uint32_t, std::vector<uint32_t>> g_free_bins;  // bucket -> addrs
static uint64_t g_heap_reused = 0, g_heap_freed = 0;
static std::unordered_set<uint32_t> g_freed_set;   // currently on a free list
static std::unordered_set<uint32_t> g_reused_set;  // has been handed out again

static inline uint32_t heap_bucket(uint32_t size) {
    uint32_t b = 32; while (b < size && b < 0x40000000u) b <<= 1; return b;
}
static const int g_heap_reuse = [] { return bo2_getenv("BO2_NO_HEAP_REUSE") ? 0 : 1; }();

// Bump-allocate `size` bytes at `align`-byte alignment, or reuse a freed block
// of a comparable size. Returns a guest address (never a host pointer), or 0 on
// exhaustion.
static void heap_trace_sp(uint32_t sp, uint32_t size, const char* what);
extern "C" State* win32_current_guest_state();

// ── 🎯 DEUX DRAPEAUX DECLARES ICI, TOUT EN HAUT, ET C'EST VOULU ──────────────
// `t_host_tid` dit « ce fil hote execute la coroutine n » ; `g_any_hosted` dit
// « au moins une coroutine a quitte le tourniquet ». Ils servent DEUX fois dans
// ce fichier — au proxy `g_cur` (ligne ~710) et au verrou d'arene juste en
// dessous — et le second est AVANT le premier dans l'ordre du fichier.
thread_local int t_host_tid = -1;   // >= 0 : ce fil hote EXECUTE cette coroutine
// ⚠️ LIEN EXTERNE DEPUIS 2026-09-21, ET C'EST LA MOITIE D'UN CORRECTIF.
// `win32_runtime.cpp` en a besoin sur le chemin d'ECRITURE invitee le plus
// chaud (`__remill_write_memory_32_slow`, cf. P615) : il doit savoir, sans
// payer un appel inter-unite ni une lecture TLS, si le fil courant peut etre un
// fil heberge. Lu d'abord, il court-circuite la lecture de `t_host_tid`, donc
// tant que rien n'est heberge le cout est UNE lecture de booleen global et une
// branche predite — le meme prix que `g_w32_fast_on` juste a cote.
bool g_any_hosted BO2_HOT_LINE = false;
// 🔴 P641 — L'IDENTITE DU FIL ORDONNANCEUR, POUR LE REPLIAGE DE lifted.o.
// Le compteur de preemption replie dans l'IR (tools/boot.py) doit, comme la voie
// C++ depuis P615, n'etre decremente QUE par le fil ordonnanceur ; sans TLS, il le
// reconnait a `tpidrro_el0` (ce que lit pthread_self() sur Darwin arm64), compare a
// ce mot. Pose par maybe_host() AVANT de lever g_any_hosted ; lu seulement quand
// g_any_hosted est vrai. Un mot perime ne donne qu'un tic manque, pas un gel : le
// test replie est `<= 0` signe.
extern "C" { uint64_t bo2_sched_tp BO2_HOT_LINE = 0; }
static inline uint64_t bo2_read_tp() {
    return bo2_read_thread_pointer();   // tpidrro_el0 sur Darwin, tpidr_el0 sur Android (bo2_platform.h)
}

// 🎯 C31 (P673) — LE FIL ORDONNANCEUR SE RECONNAIT SANS TLS. Il garde t_host_tid = -1 (seul
// host_thread_entry le pose), et son tpidrro_el0 vaut bo2_sched_tp des que g_any_hosted est vrai.
// Chaque test « ce fil est-il heberge ? » le court-circuite donc AVANT la lecture de t_host_tid :
// meme reponse, sans `_tlv_get_addr`. Sur Main, qui passe par SchedCur a chaque g_cur,
// _tlv_get_addr pesait 2,7 % du fil (sample p672 181347). Les fils heberges lisent toujours le TLS.
// ⚠️ bo2_sched_tp et g_any_hosted sont publies sans barriere : un lecteur peut voir g_any_hosted vrai
// avec bo2_sched_tp encore a 0. Sans danger tant que 0 n'est le tpidrro_el0 d'aucun fil (il tombe
// alors sur la lecture TLS) ; a revoir si la sentinelle « non pose » change.
static inline bool k32_hors_ordonnanceur() { return bo2_read_tp() != bo2_sched_tp; }

// « ce fil-ci est sorti du tourniquet ». Trois primitives d'attente en dependent
// (WFSO, Sleep, coop_yield) : voir le bloc P602 au-dessus de coop_yield().
static inline bool hosted_self() {
    return __builtin_expect(g_any_hosted, 0) && k32_hors_ordonnanceur() && t_host_tid >= 0;
}

// ── LE VERROU D'ARENE, ET POURQUOI IL EST DERRIERE LA MEME PORTE ─────────────
// `guest_alloc`/`guest_free` ne sont pas un compteur qu'on pourrait se permettre
// de fausser : ils INSERENT dans quatre conteneurs standards (`g_alloc_sizes`,
// `g_free_bins`, `g_freed_set`, `g_reused_set`). Deux fils qui inserent en meme
// temps dans un `unordered_map` ne rendent pas un mauvais compte, ils rendent une
// LISTE CHAINEE CASSEE — ou, pire et plus silencieux, la meme adresse invitee
// distribuee DEUX FOIS. Le symptome serait un pointeur valide vers de la memoire
// deja vivante ailleurs : exactement le gel qu'on a passe des jours a nommer
// (la marche de seau sans sortie de sub_87c8e0).
//
// ⚠️ C'est la piece #4 du travail multicoeur, et la note « compteurs non
// atomiques : mauvais comptes, pas corruption » est TROP OPTIMISTE pour
// l'allocateur. Corrigee ici plutot que decouverte a l'execution.
//
// Recursif par principe : `guest_alloc` appelle `heap_trace_sp`, et rien ne
// garantit qu'un futur traceur ne rappellera pas l'arene. Un auto-blocage ici
// serait indiscernable d'un gel du jeu.
//
// Le drapeau est CAPTURE a la construction : si `g_any_hosted` basculait entre
// la prise et le rendu, on rendrait un verrou qu'on n'a pas pris.
static std::recursive_mutex g_arena_mx;
struct ArenaLock {
    const bool on;
    inline ArenaLock() : on(__builtin_expect(g_any_hosted, 0)) { if (on) g_arena_mx.lock(); }
    inline ~ArenaLock() { if (on) g_arena_mx.unlock(); }
    ArenaLock(const ArenaLock&) = delete;
};

// ── 🎯 LE VERROU DES TABLES DE SECTIONS CRITIQUES, ET POURQUOI IL EST A PART ──
//
// `g_crit_sections` et `g_cs_own` sont deux `unordered_map` touches a CHAQUE
// Enter/Leave — c'est-a-dire ~80 000 fois par seconde, et 50,2 % du trafic
// d'imports au gel ([IMP-HIST]). Les proteger avec `g_arena_mx` marcherait, mais
// ferait passer tout ce trafic par le MEME verrou que l'allocateur d'arene :
// avec les quatre ouvriers JQ heberges, c'est cinq fils qui se serialisent sur
// un objet unique, et le 1,47x mesure y passerait sans que rien n'echoue.
// ⇒ verrou SEPARE, meme porte `g_any_hosted` (cout nul tant que rien n'est
//   heberge), meme capture du drapeau a la construction.
//
// ⚠️ REGLE DE TENUE, non negociable : ce verrou ne doit JAMAIS etre tenu au
// travers d'un `coop_yield_from` ni d'un `pthread_mutex_lock` de section
// invitee. Le tenir pendant une cession donnerait le verrou au FIL hote pendant
// qu'une autre coroutine du meme fil s'execute — un `recursive_mutex` la
// laisserait rentrer, donc l'exclusion serait silencieusement fausse. Toutes les
// prises ci-dessous sont donc dans une portee explicite, autour du seul acces a
// la table.
static std::recursive_mutex g_cs_mx;
struct CsLock {
    const bool on;
    inline CsLock() : on(__builtin_expect(g_any_hosted, 0)) { if (on) g_cs_mx.lock(); }
    inline ~CsLock() { if (on) g_cs_mx.unlock(); }
    CsLock(const CsLock&) = delete;
};

extern "C" uint8_t* bo2_g_base;
static uint32_t guest_alloc(uint32_t size, uint32_t align = 16) {
    ArenaLock _al;
    if (align < 1) align = 1;
    if (size == 0) size = 1;                     // distinct address per call
    if (g_heap_reuse) {
        auto it = g_free_bins.find(heap_bucket(size));
        if (it != g_free_bins.end()) {
            auto& v = it->second;
            for (size_t i = v.size(); i-- > 0; ) {
                const uint32_t a = v[i];
                const uint32_t have = g_alloc_sizes[a];
                if (have >= size && have <= 2u * size && (a & (align - 1)) == 0) {
                    v[i] = v.back(); v.pop_back();
                    g_freed_set.erase(a); g_reused_set.insert(a);
                    g_heap_reused++;
                    // Un bloc recycle n'est plus « frais » : tous les appelants (HeapAlloc avec HEAP_ZERO_MEMORY,
                    // VirtualAlloc, objets du runtime) comptent sur une memoire a zero. Sans cela le calloc du contexte
                    // libvpx rend un bloc sale, `decoder_init` vaut deja 1 et le decodeur tourne sans sa table de
                    // fonctions ([NULL-CALL] 0x9bd5b7, Z-NATIF-17d).
                    std::memset(bo2_g_base + a, 0, have);
                    return a;                    // extent unchanged; still `have` bytes
                }
            }
        }
    }
    {   // Trace HERE, not at the API shims: the block that exhausts the arena
        // arrives through some other entry point, and only the allocator sees
        // them all. The guest stack is fetched rather than passed in so no call
        // site needs to know about it.
        State* gs = win32_current_guest_state();
        heap_trace_sp(gs ? gs->gpr.rsp.dword : 0, size, "guest_alloc");
    }
    uint32_t base = (g_heap_next + (align - 1)) & ~(align - 1);
    if (base < g_heap_next) return 0;            // alignment wrapped 32-bit
    // Jump the TEB/PEB block rather than allocate over it. Written as
    // `size > HOLE_BASE - base` and not `base + size > HOLE_BASE` so a huge
    // request near the boundary cannot wrap 32-bit and silently pass.
    if (base < GUEST_TEB_HOLE_END && (base >= GUEST_TEB_HOLE_BASE ||
                                      size > GUEST_TEB_HOLE_BASE - base)) {
        base = (GUEST_TEB_HOLE_END + (align - 1)) & ~(align - 1);
        if (base < GUEST_TEB_HOLE_END) return 0; // alignment wrapped
    }
    if (base > GUEST_HEAP_END || size > GUEST_HEAP_END - base) {
        // LOUD, and in the DEFAULT build: an exhausted arena makes the game put
        // up its own "Out of memory error" and ExitProcess, which is four frames
        // away from the cause and reads like a game bug. Say it plainly, once.
        static int said = 0;
        if (!said++) {
            fprintf(stderr, "[HEAP-EXHAUSTED] guest arena full: %u MiB handed out of %u MiB, "
                            "request %u bytes align %u (freed %llu blocks, reused %llu)\n",
                    (g_heap_next - GUEST_HEAP_BASE) >> 20,
                    (GUEST_HEAP_END - GUEST_HEAP_BASE) >> 20, size, align,
                    (unsigned long long)g_heap_freed, (unsigned long long)g_heap_reused);
            // WHICH SIZES ATE IT. Without this the only thing "out of memory"
            // tells you is that something did, and a bump arena has no other
            // record. Total bytes per power-of-two bucket, biggest first.
            std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> by;  // bucket -> (bytes,count)
            for (auto& kv : g_alloc_sizes) {
                auto& e = by[heap_bucket(kv.second)];
                e.first += kv.second; e.second++;
            }
            std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> v(by.begin(), by.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
            for (size_t i = 0; i < v.size() && i < 12; i++)
                fprintf(stderr, "[HEAP-EXHAUSTED]   <=%-10u %6llu MiB in %llu live blocks "
                                "(%zu free-listed)\n",
                        v[i].first, (unsigned long long)(v[i].second.first >> 20),
                        (unsigned long long)v[i].second.second,
                        g_free_bins.count(v[i].first) ? g_free_bins[v[i].first].size() : 0);
        }
        return 0;
    }
    g_heap_next = base + size;
    g_alloc_sizes[base] = size;
    // Une ligne par 100 Mio d'arene neuve : la frise de la croissance, à lire entre les `Server:` (P1049).
    static uint32_t tas_cran = 0;
    if ((g_heap_next - GUEST_HEAP_BASE) / (100u << 20) != tas_cran) {
        tas_cran = (g_heap_next - GUEST_HEAP_BASE) / (100u << 20);
        State* gs = win32_current_guest_state();
        fprintf(stderr, "[TAS] haut=%u Mo bloc=%u esp=0x%x libres=%llu recycles=%llu\n", tas_cran * 100, size,
                gs ? gs->gpr.rsp.dword : 0, (unsigned long long)g_heap_freed, (unsigned long long)g_heap_reused);
    }
    return base;
}

// Return a block to the free list. Unknown addresses (never handed out by us,
// or already freed) are ignored — a double free must not put the same address
// in twice, or two live allocations would alias.
// Which allocation owns a guest address, and has it been freed / handed out
// again? Answers the one question a corrupted pointer poses.
//
// The lock-free job queue's nodes come from one 0x108-strided pool and its
// head/tail pointers were measured being overwritten with GUEST RETURN
// ADDRESSES — data that belongs to a coroutine stack, which is allocated from
// this same arena (nodes at 0x41394d18, the stack at 0x4138ed50, ~24 KB apart).
// That is either two live allocations overlapping, which the bump allocator
// cannot do, or one block freed while still in use and handed to something
// else, which reuse absolutely can. Naming the owning block and its state
// separates those two, and no amount of staring at the queue algorithm can.
//
// Linear over g_alloc_sizes on purpose: it is called only when a corruption has
// already been caught, never on the allocation path.

extern "C" void win32_heap_owner(uint32_t a, uint32_t* base, uint32_t* size,
                                 int* freed, int* reused) {
    uint32_t bb = 0, bs = 0;
    for (const auto& kv : g_alloc_sizes)
        if (a >= kv.first && a < kv.first + kv.second && (!bb || kv.first > bb)) {
            bb = kv.first; bs = kv.second;
        }
    if (base) *base = bb;
    if (size) *size = bs;
    if (freed)  *freed  = bb && g_freed_set.count(bb) ? 1 : 0;
    if (reused) *reused = bb && g_reused_set.count(bb) ? 1 : 0;
}

// Meme question, sous le verrou de l'arene : appelable depuis une sonde pendant que des fils
// heberges allouent (g_alloc_sizes est un unordered_map, un rehash concurrent le corrompt).
extern "C" void win32_heap_owner_locked(uint32_t a, uint32_t* base, uint32_t* size,
                                        int* freed, int* reused) {
    ArenaLock _al;
    win32_heap_owner(a, base, size, freed, reused);
}

static void guest_free(uint32_t addr) {
    ArenaLock _al;
    if (!g_heap_reuse || !addr) return;
    auto it = g_alloc_sizes.find(addr);
    if (it == g_alloc_sizes.end()) return;
    auto& v = g_free_bins[heap_bucket(it->second)];
    for (uint32_t a : v) if (a == addr) return;  // already free: ignore
    v.push_back(addr);
    g_freed_set.insert(addr);
    g_heap_freed++;
}
// Exposed so the D3D shim can return a dynamic-texture backing when the guest
// releases the texture. Without it every video frame leaks its staging buffer.
extern "C" void win32_guest_free(uint32_t addr) { guest_free(addr); }
extern "C" void win32_heap_stats(uint64_t* freed, uint64_t* reused, uint32_t* high) {
    if (freed) *freed = g_heap_freed;
    if (reused) *reused = g_heap_reused;
    if (high) *high = g_heap_next - GUEST_HEAP_BASE;
}

// Exposed to other shim TUs (e.g. Steam interface emulation in
// remaining_dlls_impl.cpp) that need small fixed guest-memory objects.
extern "C" uint32_t win32_guest_alloc(uint32_t size, uint32_t align) {
    return guest_alloc(size, align ? align : 16);
}

// --- SHIM-OWNED OBJECTS LIVE IN THEIR OWN ARENA ------------------------------
// Fake COM objects (XAudio2 voices, D3D interfaces, ...) used to be bump-
// allocated from the SAME arena as the guest's HeapAlloc/VirtualAlloc, so they
// sat interleaved between the game's own blocks. MEASURED consequence: all three
// XAudio2 voice objects had their vtable pointer zeroed while the pointer to
// them stayed valid — the signature of a neighbouring guest allocation being
// written past its end, not of a stale pointer. A bump allocator never hands the
// same address out twice, so reuse could not explain it; adjacency could.
// Keeping shim objects in a disjoint arena means a guest overrun can no longer
// reach them, and equally that our objects can never corrupt the game's heap.
// 0x18000000 is free in the guest map (stack 0x00100000, image 0x00400000+,
// cmdline scratch 0x0F000000, guest heap 0x20000000+, TEB/PEB 0x7FFD0000+).
static constexpr uint32_t SHIM_ARENA_BASE = 0x18000000u;
static constexpr uint32_t SHIM_ARENA_END  = 0x1FF00000u;
static uint32_t g_shim_next = SHIM_ARENA_BASE;
extern "C" uint32_t win32_shim_alloc(uint32_t size, uint32_t align) {
    ArenaLock _al;
    if (align < 1) align = 16;
    if (size == 0) size = 1;
    uint32_t base = (g_shim_next + (align - 1)) & ~(align - 1);
    if (base < g_shim_next || base > SHIM_ARENA_END ||
        size > SHIM_ARENA_END - base) return 0;
    g_shim_next = base + size;
    // BO2_SHIMLOG=1 — who owns a shim-arena address? Exactly one object in a
    // whole run gets BOUND as a texture and never has pixels (0x180001c0, ~2
    // binds a frame, 6400 over a run) while every other texture resolves. It
    // lives in this arena, not the guest heap, so it was never made by
    // CreateTexture2D — and naming its allocation names what the black area of
    // the menu actually is. Caller-side host frames included: the arena has one
    // entry point and several very different users.
    if (bo2_getenv("BO2_SHIMLOG")) {
        static int n = 0;
        if (++n <= 64) {
            fprintf(stderr, "[SHIM-ALLOC #%d] 0x%08x size=%u align=%u tid=%d\n",
                    n, base, size, align, -1);
            void* fr[5]; int nf = backtrace(fr, 5);
            char** sy = backtrace_symbols(fr, nf);
            if (sy) { for (int i = 1; i < nf; i++) fprintf(stderr, "  [sa] %s\n", sy[i]); free(sy); }
        }
    }
    return base;
}

// --- thread-local Win32 state ---
static thread_local uint32_t t_last_error = 0;
static uint32_t t_process_heap = 1;  // fake handle
static char t_cmdline[] = "t6sp.exe\0";
// 🔴 P628/E8 — L'ALLOCATEUR DE POIGNEES EST UN GLOBAL PARTAGE, ET SON `++`
// N'ETAIT PAS ATOMIQUE. Le prefixe `t_` ment : ce n'est pas un `thread_local`,
// c'est un global, declare `extern` par user32_impl.cpp et remaining_dlls_impl.cpp
// (le commentaire d'origine le disait a moitie : « shared with user32_impl.cpp »).
// Tant que tout l'invite tournait sur UN fil hote, `++` etait sur par
// construction. Des que deux fils hotes creent un evenement ou un fil en meme
// temps, une incrementation se perd et DEUX objets recoivent la MEME poignee :
// le second ecrase le premier dans `g_events`, et un fil attend alors un
// evenement qui existe mais qui appartient a autre chose. Introuvable au
// debogueur, et exactement le genre de defaut qui a fait lire P615 « heberger
// est instable ».
// ⇒ `fetch_add` relaxe : aucune barriere n'est requise, on ne veut qu'UNICITE.
//   Cout : un `ldaddal`-sans-barriere par CREATION de poignee, quelques milliers
//   par run — invisible, et paye aussi dans le regime par defaut. Le rendre
//   conditionnel a `g_any_hosted` couterait plus cher en branchement qu'il ne
//   rapporte, et laisserait deux semantiques a raisonner.
std::atomic<uint32_t> t_fake_handle_counter{0x10000};  // shared with user32_impl.cpp

// --- HEAP ---
static void impl_GetProcessHeap(State& s) {
    s.gpr.rax.dword = t_process_heap;
    win32_stdcall_ret(s, 0);
}

// BO2_HEAP_TRACE=<min bytes> — name the guest call site of every allocation at
// or above that size, and count them per site. A bump arena keeps no history, so
// "624 MiB in 473 live blocks of 1.35 MB" says WHAT ate the heap but not WHO;
// the return address on the guest stack does, and it costs one read.
static void heap_trace_sp(uint32_t sp, uint32_t size, const char* what) {
    static const uint32_t min_sz = [] {
        const char* e = bo2_getenv("BO2_HEAP_TRACE");
        return e ? (uint32_t)strtoul(e, nullptr, 0) : 0u; }();
    if (!min_sz || size < min_sz) return;
    static std::unordered_map<uint32_t, uint64_t> sites;   // guest ret -> count
    if (!sp) return;
    const uint32_t ret = win32_mem_read32(sp);
    uint64_t& n = sites[ret];
    if (++n <= 3 || (n % 100) == 0) {
        fprintf(stderr, "[HEAP-TRACE] %s %u bytes from ret=0x%08x (#%llu from this site, "
                        "%zu distinct sites)\n",
                what, size, ret, (unsigned long long)n, sites.size());
        // The immediate return address is usually inside the CRT's malloc, which
        // names nothing. Scan up the guest stack for the game-level frames.
        int shown = 0;
        for (uint32_t a = sp; a < sp + 0x200 && shown < 8; a += 4) {
            uint32_t v = win32_mem_read32(a);
            if (v >= 0x401000u && v < 0xa32000u) {
                fprintf(stderr, "[HEAP-TRACE]   +%03x -> 0x%08x\n", a - sp, v);
                shown++;
            }
        }
    }
}
static void impl_HeapAlloc(State& s) {
    // HeapAlloc(handle, flags, size) — stdcall 3 args. Serve a guest address;
    // fresh g_mem bytes read as 0, so HEAP_ZERO_MEMORY needs no extra work.
    uint32_t size = win32_arg(s, 2);
    heap_trace_sp(s.gpr.rsp.dword, size, "HeapAlloc");
    s.gpr.rax.dword = guest_alloc(size, 16);
    win32_stdcall_ret(s, 3);
}

static void impl_HeapFree(State& s) {
    // HeapFree(handle, flags, ptr) — stdcall 3 args. Returns the block to the
    // free list (see guest_alloc). Must NOT free() the arg — it is a guest
    // address, not a host pointer.
    guest_free(win32_arg(s, 2));
    s.gpr.rax.dword = 1; // BOOL success
    win32_stdcall_ret(s, 3);
}

static void impl_HeapReAlloc(State& s) {
    // HeapReAlloc(handle, flags, ptr, size) — stdcall 4 args.
    uint32_t ptr  = win32_arg(s, 2);
    uint32_t size = win32_arg(s, 3);
    if (ptr == 0) {  // documented: behaves like HeapAlloc
        s.gpr.rax.dword = guest_alloc(size, 16);
        win32_stdcall_ret(s, 4);
        return;
    }
    uint32_t g = guest_alloc(size, 16);
    if (g) {
        uint32_t oldsz;
        {   // P661/C15 : lire la table sous le verrou de l'arene (un autre fil y insere)
            ArenaLock _al;
            auto it = g_alloc_sizes.find(ptr);
            oldsz = (it != g_alloc_sizes.end()) ? it->second : 0;
        }
        uint32_t n = oldsz < size ? oldsz : size;
        for (uint32_t i = 0; i < n; i++) win32_mem_write8(g + i, win32_mem_read8(ptr + i));
        // tail bytes past the copy are implicitly zero (fresh g_mem).
        if (g != ptr) guest_free(ptr);           // the old block is dead now
    }
    s.gpr.rax.dword = g;
    win32_stdcall_ret(s, 4);
}

static void impl_HeapCreate(State& s) {
    s.gpr.rax.dword = ++t_fake_handle_counter;
    win32_stdcall_ret(s, 3);
}

static void impl_HeapDestroy(State& s) {
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 1);
}

static void impl_HeapSize(State& s) {
    // HeapSize(handle, flags, ptr) — stdcall 3 args. Return the recorded size,
    // or (SIZE_T)-1 for an unknown block (the Win32 failure value).
    uint32_t ptr = win32_arg(s, 2);
    ArenaLock _al;                                   // P661/C15 : un autre fil y insere
    auto it = g_alloc_sizes.find(ptr);
    s.gpr.rax.dword = (it != g_alloc_sizes.end()) ? it->second : (uint32_t)-1;
    win32_stdcall_ret(s, 3);
}

// --- THREADING ---
// Guest threads are coroutines on ONE host thread, so the host thread id is
// identical for all of them — the game's "am I thread [0x2925588]?" gates
// (0x80df30/0x502ff0) then passed on EVERY thread. Return a distinct id per
// guest coroutine instead. Defined after the scheduler (needs g_cur).
static void impl_GetCurrentThreadId(State& s);

static void impl_GetCurrentProcessId(State& s) {
    s.gpr.rax.dword = (uint32_t)getpid();
    win32_stdcall_ret(s, 0);
}

static void impl_GetCurrentThread(State& s) {
    s.gpr.rax.dword = 0xFFFFFFFE; // pseudo handle
    win32_stdcall_ret(s, 0);
}

static void impl_GetCurrentProcess(State& s) {
    s.gpr.rax.dword = 0xFFFFFFFF; // pseudo handle
    win32_stdcall_ret(s, 0);
}

static void coop_yield();   // fwd — cooperative scheduler (defined below)
static void coop_yield_from(const char* why);   // fwd — same, with attribution
static uint32_t ordo_attendre_jusqua(const char* why, uint64_t deadline);   // fwd — P796
extern "C" void bo2_xaudio_pump(State* s);      // audio callback flush (shim)
extern "C" void bo2_dump_asset_db(const char*);  // win32_runtime.cpp
extern int g_bo2_sync_zone;                     // win32_runtime.cpp: armed by bo2_spmap
extern "C" int win32_current_tid();   // fwd — defined with the scheduler below
extern "C" uint64_t bo2_mono_ns(void);  // fwd — file scope: `extern "C"` is a
                                        // linkage-spec, illegal inside a function.

static void impl_Sleep(State& s) {
    static DiagCounter<12> s_sc;   // P660/C14 : l'unordered_map bouclait dans un rehash (diag_counter.h)
    uint32_t ret = win32_mem_read32(s.gpr.rsp.dword);
    const uint32_t ms_arg = win32_arg(s, 0);   // lu AVANT win32_stdcall_ret, qui depile
    if (s_sc.bump(ret) == 0) {
        static std::atomic<bool> dit{false};
        if (!dit.exchange(true)) {
            fprintf(stderr, "[C14] compteurs de diagnostic sans verrou : Sleep, SetLastError, sync_log, DBWAIT\n");
            fprintf(stderr, "[C15] courses des fils heberges : vsnprintf, inflate, HeapSize/ReAlloc, mutex, "
                            "GetProcAddress, VM, UNK-CALLER, Steam, XAudio\n");
        }
        fprintf(stderr, "[Sleep] caller-ret=0x%08x esp=0x%08x\n", ret, s.gpr.rsp.dword);
    }

    // 🎯 BO2_GROUP_WATCH=1 — THE JOB-GROUP PENDING COUNTERS MAIN NEVER SEES REACH 0.
    //
    // The freeze differential (one profiled replay, rendering window vs frozen
    // window of the SAME run) showed main starting to sleep at 0x5cb755 — a site
    // it never used while rendering — and stopping ALL lock work. 0x5cb6c0 is
    // WaitForJobGroup(group): `while (IsGroupActive(group)) { drain; Sleep; }`,
    // and IsGroupActive (0x8f9e40) is just
    //     eax = [group] | [group+4];  active = (eax != 0)
    // so a group is a PAIR of pending counters and main blocks until both are 0.
    //
    // 0x65b4d0 computes the group as obj + index*8 + 0x210 and is called
    // DIRECTLY, so the crumb ring cannot see it (GUIDE §8: direct lifted calls
    // bypass the dispatcher). But this Sleep is reached by `call ebp` with
    // ebp = the Sleep IAT slot, and edi STILL HOLDS THE GROUP — it is loaded at
    // 0x5cb6c2 from arg0 and pushed at 0x5cb6c1, so it survives the loop. Two
    // reads and no offset guesswork.
    if (ret == 0x5cb755u || ret == 0x5cb715u) {
        static const int gw = bo2_getenv("BO2_GROUP_WATCH")
                            ? atoi(bo2_getenv("BO2_GROUP_WATCH")) : 0;
        if (gw) {
            const uint32_t grp = s.gpr.rdi.dword;
            const uint32_t lo  = win32_mem_read32(grp);
            const uint32_t hi  = win32_mem_read32(grp + 4);
            static uint64_t n = 0;
            static uint32_t last_lo = 0xffffffffu, last_hi = 0xffffffffu;
            const bool moved = (lo != last_lo || hi != last_hi);
            // Print the first few, every change, and then on a cadence — a
            // counter that STOPS MOVING is the whole hypothesis, so a change is
            // the event worth catching, not the repetition.
            if (++n <= 6 || moved || (n % 5000) == 0)
                fprintf(stderr, "[GROUP] #%llu group=0x%08x pending=[%u,%u]%s tid=%d\n",
                        (unsigned long long)n, grp, lo, hi,
                        moved ? "  <<< CHANGED" : "", win32_current_tid());
            last_lo = lo; last_hi = hi;
        }
    }
    // 🎯 BO2_GROUP_WATCH — deuxieme site : le Sleep(0) DE Sys_WaitForJobs.
    //
    // runs/w32_gw_d a montre que WaitForJobGroup (0x5cb6c0) n'est PAS la ou
    // Main se perd : son Sleep tire 1213 fois puis s'arrete net, la derniere
    // valeur lue etant `group=0x00ebb660 pending=[0,0]`, c'est-a-dire un groupe
    // INACTIF dont la boucle sort. Ce qui ne rend jamais la main, c'est le
    // DRAIN qu'elle appelle avant de dormir : 0x8fb9d0 (Sys_WaitForJobs), dont
    // la pile au [RUNAWAY] montre quatre niveaux imbriques et qui execute
    // `updatesound_worker` en boucle (0x553600 pousse la chaine 0xac201c).
    //
    // 0x8fb9d0 attend que la PAIRE 64 bits [edi]/[edi+4] rejoigne la cible
    // (arg1, arg2) :
    //     0x8fba25  esi = [ebp]              ; ebp == edi
    //     0x8fba37  call InterlockedExchange ; barriere
    //     0x8fba3d  eax = [edi+4] + esi ; jne -> executer un job de plus
    //     0x8fba47  [edi+4] vs arg2, puis [edi] vs arg1 -> jbe = SORTIR
    // Son Sleep(0) est `push 0 / call [0xa3b0f8]` en 0x8fba7d, donc au moment
    // ou on est ici esp pointe la case de retour 0x8fba83 et l'esp d'entree de
    // la fonction vaut esp + 0x2c (sub 0x14 + 5 push + 2 pousses d'arguments).
    // edi est preserve par l'ABI a travers l'appel a 0x8fb170.
    //
    // ⚠️ Ce probe ne juge rien : il RECOPIE quatre mots. Un compteur qui cesse
    // de bouger est toute l'hypothese, donc on imprime les premiers, chaque
    // CHANGEMENT, puis une cadence — et la derniere ligne avant la mort est la
    // reponse.
    if (ret == 0x8fba83u) {
        static const int gw = bo2_getenv("BO2_GROUP_WATCH")
                            ? atoi(bo2_getenv("BO2_GROUP_WATCH")) : 0;
        if (gw) {
            const uint32_t esp = s.gpr.rsp.dword;      // case de retour
            const uint32_t E   = esp + 0x2cu;          // esp d'entree de 0x8fb9d0
            const uint32_t a0  = win32_mem_read32(E + 4);
            const uint32_t a1  = win32_mem_read32(E + 8);
            const uint32_t a2  = win32_mem_read32(E + 12);
            const uint32_t grp = a0 ? a0 : 0x00ea79c0u;
            const uint32_t lo  = win32_mem_read32(grp);
            const uint32_t hi  = win32_mem_read32(grp + 4);
            static uint64_t n = 0;
            static uint32_t l_lo = 0xffffffffu, l_hi = 0xffffffffu, l_g = 0;
            const bool moved = (lo != l_lo || hi != l_hi || grp != l_g);
            if (++n <= 6 || moved || (n % 20000) == 0)
                fprintf(stderr, "[JOBWAIT] #%llu grp=0x%08x cnt=[%u,%u] "
                        "target=[%u,%u] edi=0x%08x tid=%d%s\n",
                        (unsigned long long)n, grp, lo, hi, a1, a2,
                        s.gpr.rdi.dword, win32_current_tid(),
                        moved ? "  <<< CHANGED" : "");
            l_lo = lo; l_hi = hi; l_g = grp;
        }
    }
    // DB_FindXAssetHeader (FUN_004637c0) internal wait: it calls the Sleep
    // wrapper FUN_0058d810, so impl_Sleep's immediate ret is 0x58d81b (inside
    // the wrapper). Scan the whole guest stack for asset-name pointers (a
    // pointer to printable ASCII) to see what a thread is stuck waiting for.
    // Capped; one line per distinct (tid,name).
    if (ret == 0x58d81bu) {
        static DiagCounter<12> s_seen;   // P660/C14
        int tid = win32_current_tid();
        uint32_t sp = s.gpr.rsp.dword;
        // The name is formatted into a stack buffer, so scan the stack BYTES
        // for inline ASCII runs (not pointers-to-ASCII). A run with '/' or '_'
        // and length >= 6 is an asset path.
        uint32_t a = sp;
        while (a < sp + 0x900) {
            char nm[64]; int n = 0; bool named = false;
            uint32_t start = a;
            while (n < 63) {
                uint8_t c = win32_mem_read8(a);
                if (c < 0x20 || c > 0x7e) break;
                if (c == '/' || c == '_') named = true;
                nm[n++] = (char)c; a++;
            }
            nm[n] = 0;
            if (n >= 6 && named && s_seen.bump(((uint64_t)tid<<20)^start) == 0) {
                fprintf(stderr, "[DBWAIT] tid=%d @0x%08x=\"%s\"\n", tid, start, nm);
                // 🎯 DUMP THE ASSET DATABASE THE MOMENT A .gsc WAIT APPEARS.
                //
                // The campaign load ends with main spinning in
                // DB_FindXAssetHeader for maps/afghanistan.gsc while the script
                // VM reports `last 0 opcodes` — it has never executed an
                // instruction. Whether that asset is in the database is the
                // whole question, and bo2_dump_asset_db() answers it.
                //
                // It used to hang off [HARDCAP], and then off [RUNAWAY]. Neither
                // is reachable here: the game draws its LOADING SCREEN while it
                // waits, so frames keep presenting and the runaway guard — which
                // keys on "no new frame" precisely so a healthy long run is not
                // killed — correctly never fires. A dump that needs a failure
                // guard to fire cannot observe a state that is not a failure.
                // ⚠️ NOT "the first .gsc wait" — that is the FRONTEND's
                // (maps/createart/frontend_art.gsc), and it fires ~1,500 frames
                // before the campaign map exists, so the dump described the
                // frontend zone and answered a question nobody asked. Dump on
                // the first .gsc wait that is NOT a frontend one; the campaign
                // script's own wait is the event of interest.
                // ⚠️ AND NOT a name filter either. The [DBWAIT] scanner reads
                // ASCII runs off the stack, so the names arrive TRUNCATED —
                // "maps/createart/frontend_art.gsc" shows up as "_art.gsc",
                // which contains no "frontend" and sailed straight through the
                // exclusion. Gate on engine STATE instead: g_bo2_sync_zone is
                // armed by bo2_spmap the moment a non-frontend level is
                // requested, which is exactly "the campaign has started".
                if (g_bo2_sync_zone && strstr(nm, ".gsc")) {
                    static int s_gsc_dumped = 0;
                    if (s_gsc_dumped++ == 0) {
                        fprintf(stderr, "[DBWAIT] first CAMPAIGN .gsc wait (\"%s\") "
                                        "— dumping the asset DB\n", nm);
                        bo2_dump_asset_db("gsc-wait");
                    }
                }
            }
            a = (n > 0) ? a + 1 : a + 4;
        }
    }
    win32_stdcall_ret(s, 1);
    // 🎯 BO2_SLEEP_REAL=1 — Sleep(n) DOIT RENDRE n MILLISECONDES REELLES.
    //
    // Le defaut ci-dessous rend UN tour d'ordonnanceur, quel que soit n. C'est la
    // meme confusion travail/temps que les trois sites de WaitForSingleObject, et
    // c'est la derniere qui reste : une boucle d'abandon borne
    //     for (i = 0; i < 100; i++) { if (fini) break; Sleep(10); }
    // devait durer une seconde et dure quelques microsecondes, donc le code
    // continue avec `fini` faux. Un invite plus rapide ne fait qu'aggraver le
    // rapport travail/temps a l'endroit precis ou le jeu avait demande a attendre.
    //
    // Ce que ca doit reparer, mesure et pas suppose (BO2_PHYSSTACK, Etape 34) :
    //   REUSSITE : image 1110 -> "cinematicTextures" depile, PUIS image 1505 ->
    //              les trois blocs LUI depiles. Ordre LIFO respecte.
    //   ECHEC    : image 1200 depassee SANS que "cinematicTextures" soit depile,
    //              puis liberation d'un bloc du dessous -> Com_Error.
    // A progression invitee EGALE, la liberation n'a pas lieu : elle depend du
    // temps reel. Rendre son temps a Sleep rend au film le temps de finir.
    //
    // Sleep(0) reste un simple yield (c'est sa semantique Windows). Le plafond
    // evite qu'une valeur aberrante gele le processus : l'ordonnanceur est
    // mono-thread hote, une attente de 10 s bloquerait TOUT.
    // 🎯 MEME RAISON QUE WFSO CI-DESSUS : un fil heberge n'a pas de tourniquet,
    // donc `Sleep(n)` = « un yield » lui coute un coeur entier a tourner. Sur un
    // fil hote, personne n'attend qu'on lui rende la main : on dort pour de vrai.
    if (hosted_self() && ms_arg > 0) {
        const uint32_t cap = ms_arg < 250u ? ms_arg : 250u;
        struct timespec ts = { (time_t)(cap / 1000u), (long)((cap % 1000u) * 1000000ul) };
        nanosleep(&ts, nullptr);   // (pas de comptage ici : g_yield_* est declare plus bas)
        return;
    }
    // 🎯 P649/C3 — ARME PAR DEFAUT (P634, jamais mesure, reserve le 2026-09-21).
    // Com_Frame (0x80aaf0) borne sa frame par `Sleep(1)` jusqu'a 50 fois
    // (0x80ab90-0x80abc6), puis ramene msec=0 a 1 (0x80abce). Serveur heberge, le
    // tour est quasi vide : 50 tours < 1 ms, donc 1 ms de jeu par Com_Frame et
    // ~2 700 Com_Frame/s. Le monde court a 2,7x, et SV_CheckTimeouts (0x85984c),
    // qui compte en svs.time, lache le client local : « Server Disconnected - Timed
    // out » (P648, runs 040828 et 042311). Sous Windows, Sleep(1) dort >= 1 ms.
    // BO2_SLEEP_REAL=0 desarme (A/B seulement).
    {
        static const bool sleep_real = [] {
            const char* e = bo2_getenv("BO2_SLEEP_REAL");
            return !(e && atoi(e) == 0);
        }();
        if (sleep_real && ms_arg > 0) {
            const uint32_t cap = ms_arg < 250u ? ms_arg : 250u;
            const uint64_t t0 = bo2_mono_ns();
            const uint64_t deadline = t0 + (uint64_t)cap * 1000000ull;
            const uint32_t tours = ordo_attendre_jusqua("Sleep-real", deadline);   // P796
            static uint64_t ns = 0;
            const uint64_t k = ++ns;
            if ((k & (k - 1)) == 0)
                fprintf(stderr, "[SLEEP-REAL #%llu] tid=%d ms=%u tours=%u reel=%.2f ms\n",
                        (unsigned long long)k, win32_current_tid(), ms_arg, tours,
                        (double)(bo2_mono_ns() - t0) / 1e6);
            return;
        }
    }
    coop_yield_from("Sleep");  // Sleep(n) = cooperative yield to other guest threads
}

// P773 : attendre en temps réel jusqu'à `echeance` (horloge bo2_mono_ns), avec la règle de Sleep(n) ci-dessus :
// un fil hébergé dort pour de vrai, un fil du tourniquet cède jusqu'à l'échéance. Sert à Present(SyncInterval).
extern "C" void bo2_attendre_jusqua_ns(uint64_t echeance) {
    if (hosted_self()) {
        const uint64_t now = bo2_mono_ns();
        if (echeance > now) {
            const uint64_t d = echeance - now;
            struct timespec ts = { (time_t)(d / 1000000000ull), (long)(d % 1000000000ull) };
            nanosleep(&ts, nullptr);
        }
        return;
    }
    if (bo2_mono_ns() < echeance) ordo_attendre_jusqua("Present-sync", echeance);   // P796
}

// --- Synchronous guest-thread model ---
// We have no real guest threads; the game creates a worker and the main thread
// then polls (Sleep) for a flag/event the worker sets. Run the worker proc to
// completion on a PRIVATE guest stack, saving/restoring the caller's State — the
// worker's writes to shared guest memory (the awaited event/flag) persist, so
// the main thread's next poll observes them. Works for BOUNDED init threads; a
// persistent-worker proc (infinite loop) will not return and hangs here —
// cooperative scheduling is the eventual fix. (CREATE_SUSPENDED threads run at
// ResumeThread.)
extern "C" bool win32_run_lifted(State* s, uint32_t va);
extern "C" bool win32_lifted_has(uint32_t va);   // "is there a block here?" — no run
extern "C" void win32_handshake_note(const char*, uint32_t);
extern uint32_t g_last_ret_pc;   // set by __remill_function_return (win32_runtime.cpp)
// C41 : la remontee lit `State::_0` (le dernier `ret` du fil), plus cette globale (P644).
extern "C" uint32_t win32_guest_alloc(uint32_t size, uint32_t align);

// --- Cooperative coroutine scheduler (ucontext) ---
// The title runs a thread pool: workers block (WaitForSingleObject) on job events;
// the main thread posts jobs (SetEvent) + waits for completion. Stub waits that
// return immediately make workers busy-loop and never do the job (e.g. loading
// fastfiles), so the asset DB stays empty and the title exits at config bootstrap.
// Model guest threads as coroutines that YIELD at blocking points (WaitForSingleObject
// on an unsignaled event, Sleep, or waiting on a thread handle). Each coroutine's
// guest State lives on its own host stack, so no explicit per-thread State is needed.
enum { T_RUN = 0, T_BLOCKED = 1, T_DONE = 2, T_SUSPENDED = 3 };
struct GThread {
    ucontext_t ctx;        // C19 : ne sert plus qu'a la PREMIERE entree (pile neuve de makecontext)
    jmp_buf sw_jb;         // C19 : contexte du tourniquet, sauve par _setjmp, sans masque de signaux
    bool sw_live = false;  // sw_jb est valide : la coroutine a deja cede au moins une fois
    char* hstack;          // host coroutine stack (null for the main thread)
    int status;
    uint32_t proc, param;
    uint32_t wait_h;       // handle it's blocked on (0 = none)
    bool wake_token;       // PulseEvent delivery: woken-by-pulse, consume in WFSO
    uint32_t tls_va;       // guest VA of this thread's TLS slot array (fs:[0x2c])
    uint32_t fls[128];     // PER-GUEST-THREAD FLS slots (API-only access)
    State* cur_state;      // the State this coroutine runs on (on its own stack)
    jmp_buf dispatch_jb;   // host setjmp target for guest longjmp (Lua pcall unwind)
    bool dispatch_armed;   // dispatch_jb is valid (setjmp'd) for this thread
    uint32_t resume_pc;    // guest EIP to re-dispatch after a longjmp lands
    uint32_t unwind_env;   // the jmpbuf that longjmp was handed (BO2_UNWIND_LOG)
    bool unwinding;        // resumed from a longjmp: walk the guest return chain
    uint64_t wake_at;      // virtual-tick deadline for a FINITE-timeout wait (0 = none)
    uint32_t guest_stack;  // base of the guest stack thread_trampoline gave it
    uint32_t guest_stack_size;
    uint32_t req_stack;    // dwStackSize the guest asked CreateThread for
    // 🎯 THE GSC VM'S BUILTIN-CALL SCRATCH, SAVED PER COROUTINE. Opcode 0x29
    // stores the operand-stack pointer and a "a result was produced" word in the
    // VM's per-GSC-thread block, calls a native builtin, then reads them back.
    // Two guest coroutines can be inside that window with the SAME GSC thread
    // index, and then one consumes the other's flag — measured: coroutine 0 set
    // [0x2f3b8e0]=0xffffffff, coroutine 12 ran the void builtin `setdvar`, its
    // handler took the `sub esi,8` branch, and the operand stack was one slot
    // short from then on.
    // Windows never interleaves there. Saving these across a coroutine switch is
    // the faithful equivalent and, unlike refusing to yield, it does not perturb
    // the schedule at all.
    uint32_t gsc_scratch[8];
    bool     gsc_saved;
    // 🎯 HEBERGEE : cette coroutine tourne sur son PROPRE fil hote, pas dans
    // le tourniquet. Elle en sort donc completement — l'ordonnanceur ne doit
    // plus jamais l'elire, sinon deux fils executeraient le meme State.
    bool         hosted = false;
    pthread_t    hthread = 0;
    // C25 : ou un SetEvent/PulseEvent reveille ce fil heberge bloque dans une attente.
    dispatch_semaphore_t hsem = nullptr;
    // P796 : echeance (bo2_mono_ns) d'un Sleep(n>0) ou d'un Present-sync sur le tourniquet, 0 sinon.
    uint64_t     sleep_until_ns = 0;
    // P800 : echeance (bo2_mono_ns) d'un WFSO a delai fini sur un evenement, sur le tourniquet ; 0 sinon.
    uint64_t     wfso_until_ns = 0;
    // P803 : nPriority du dernier SetThreadPriority (THREAD_PRIORITY_*, 0 = NORMAL, 15 = TIME_CRITICAL).
    int32_t      prio = 0;
    // Z-NATIF-12e : ce que la case garde pour le fil suivant (spawn_thread), et ce qui autorise a la reprendre.
    uint32_t     guest_teb = 0, guest_tls_blk = 0;
    bool         handle_closed = false;   // le jeu a ferme la poignee du fil (impl_CloseHandle)
};
static std::vector<GThread*> g_threads;    // [0] = main thread

// ── 🎯 P651/C6 — LE NOMBRE DE FILS SE PUBLIE, IL NE SE LIT PAS DANS LE VECTEUR ──
// Mesure (runs/p651_…_112637) : le fil de rendu heberge cree le fil WebM
// (CreateThread param 0xf) pendant que Main, sur le fil ordonnanceur, parcourt
// g_threads dans coop_yield. `push_back` ecrit l'element PUIS la fin du
// vecteur, en ecritures simples ; sur ARM (pas de TSO) l'autre cœur peut voir
// la nouvelle taille avant le nouveau pointeur, et lit un GThread* nul :
// SIGSEGV fault_addr=0x378 dans coop_yield, tid 0, 12 images apres le lancement.
// Sous Windows, CreateThread est sur depuis n'importe quel fil.
// ⇒ Les ecrivains (sched_init, spawn_thread) s'excluent sous g_spawn_mx et
//   publient la taille en RELEASE apres l'element ; tout lecteur passe par
//   sched_nthreads() (ACQUIRE) et ne voit donc jamais un indice dont le pointeur,
//   et le GThread qu'il designe, ne sont pas encore visibles.
static std::mutex          g_spawn_mx;
static std::atomic<size_t> g_threads_pub{0};
static inline size_t sched_nthreads() { return g_threads_pub.load(std::memory_order_acquire); }

// ── 🎯 LE TID COURANT DOIT SURVIVRE A UN SECOND FIL HOTE ────────────────────
//
// `g_cur` etait deux choses a la fois : le CURSEUR du tourniquet coopératif, et
// la reponse a « quelle coroutine invitee execute en ce moment ». Les deux
// coincident tant qu'il n'y a qu'un fil hote. Des qu'une coroutine sort sur son
// propre std::thread elles divergent : le tourniquet continue d'elire des
// coroutines sur SON fil pendant que le fil heberge execute la sienne. Un shim
// appele depuis le fil heberge lirait alors le tid de quelqu'un d'autre — et
// rendrait un mauvais TLS, un mauvais FLS, une mauvaise profondeur de section
// critique, SANS RIEN DIRE.
//
// 141 sites lisent ce nom dans ce fichier. Les editer un par un, c'est en
// rater un. On garde donc le NOM et on change ce qu'il DESIGNE : un proxy qui
// rend le tid heberge de CE fil hote quand il y en a un, et le curseur du
// tourniquet sinon.
//
// ⚠️ LA PROPRIETE QUI REND CE PAS SUR : tant qu'aucune coroutine n'est
// hebergee, `t_host_tid` vaut -1 sur tous les fils et le proxy rend exactement
// `g_sched_cur`. Le comportement par defaut est identique BIT POUR BIT, et ca
// se verifie a l'execution (voir le temoin [HOSTTID] plus bas).
//
// Les affectations (`g_cur = i`) ecrivent le curseur du tourniquet : c'est bien
// ce qu'on veut, seul l'ordonnanceur en fait et seulement sur son propre fil.
static int g_sched_cur = 0;
// `t_host_tid` et `g_any_hosted` sont declares tout en haut du fichier (avant
// `guest_alloc`, qui en a besoin pour son verrou d'arene).
// ⚠️ LA PORTE, ET POURQUOI ELLE EXISTE. Sans elle, les 141 lectures de `g_cur`
// feraient chacune un acces `thread_local` — y compris dans le tourniquet
// (4 251 fois/s) et dans les sections critiques (~80 000 appels/s). Un acces TLS
// n'est pas garanti gratuit sur macOS. Cette porte rend le cout NUL par
// construction quand rien n'est heberge, au lieu de l'esperer negligeable.
struct SchedCur {
    inline operator int() const {
        return (__builtin_expect(g_any_hosted, 0) && k32_hors_ordonnanceur() && t_host_tid >= 0)
                   ? t_host_tid
                                                                      : g_sched_cur;
    }
    inline SchedCur& operator=(int v) { g_sched_cur = v; return *this; }
};
static SchedCur g_cur;
static int g_hosted_n = 0;   // combien de coroutines tournent sur leur propre fil hote
static bool g_sched_init = false;
static const uint32_t MAIN_TEB = 0x7FFD0000u;
static const size_t COSTACK = 8u * 1024 * 1024;

// `polled`: some thread has waited on this handle with a FINITE timeout, i.e.
// it checks the event and carries on rather than blocking. Such a waiter is not
// on the blocked list, so waking it is not enough — the signaller must also give
// up the CPU or it can move the world out from under the poller. That is the
// deadlock impl_SetEvent's yield was added for. Learning WHICH handles those are
// from what actually happens beats both hardcoding a list and yielding on every
// SetEvent: the Sound Mix loop signals ~4.5 times per iteration on handles
// nobody polls, and each of those cost it a whole scheduler rotation — measured,
// it was pinned at ~30 of the 187.5 mix iterations a second the device needs.
// `signalers`: bitmask of the guest tids that have EVER SetEvent this handle.
// Recorded because a coroutine that dies mid-job takes its future signals with
// it, and the only way to tell "nobody has signalled yet" from "the only thread
// that ever signals this is gone" is to know who the signaller was. See
// win32_orphaned_events below — 2026-08-01.
struct GEvent { bool manual; bool signaled; bool polled; uint32_t signalers;
                // 🎯 BO2_PULSE_SEQ — a pulse a POLLER can still observe.
                // PulseEvent wakes only T_BLOCKED threads and then clears
                // `signaled` in the same breath, so it leaves NO state behind.
                // A 1 ms poller (WFSO(h,1)) is therefore the one thing that can
                // never see it: it is not parked at the instant of the pulse,
                // and there is nothing left to find afterwards. Measured on
                // angola: event 0x10028, 24 pulses, woke=0 on every one, with
                // tids 6/7/8 stuck on it and the frame loop stopped.
                // A monotonic counter closes the window without changing Windows
                // semantics for a thread that genuinely is not waiting: only a
                // waiter that was ALREADY INSIDE its wait when the pulse landed
                // sees the sequence move.
                uint64_t pulse_seq; };
static std::unordered_map<uint32_t, GEvent> g_events;      // handle -> event
static std::unordered_map<uint32_t, int> g_thread_handle;  // handle -> g_threads index

// ── 🔴 P628/E3 — LES DEUX TABLES DE POIGNEES SONT PARTAGEES ENTRE FILS HOTES ──
//
// `g_events` et `g_thread_handle` sont ecrites par `impl_CreateEvent` /
// `spawn_thread` et lues par SetEvent, PulseEvent, ResetEvent et WFSO — donc par
// TOUT fil invite, coroutine ou fil hote. Une insertion peut declencher un
// REHACHAGE pendant qu'un autre fil marche un seau : lecture de memoire liberee,
// et le symptome serait une poignee « introuvable » au hasard, c'est-a-dire un
// fil qui attend un evenement qui existe. Introuvable au debogueur.
//
// 🎯 CE QUI REND LE CORRECTIF BON MARCHE : la stabilite des NŒUDS d'un
// `unordered_map`. Un rehachage invalide les ITERATEURS mais PAS les REFERENCES
// ni les pointeurs vers les elements ([unord.req]). Les appelants gardent donc
// un `GEvent*` en dehors du verrou et continuent de travailler dessus — c'est
// deja ce que fait `:3523`, dont le commentaire dit exactement ca. Il suffit de
// proteger la RECHERCHE.
//
// ⚠️ Le verrou ne protege PAS le contenu d'un GEvent (les champs `signaled`,
// `polled`, `pulse_seq`). Ce sont des mots ecrits par un fil et lus par un
// autre ; leur course est benigne au sens ou elle ne corrompt aucune structure,
// et la regler demanderait des atomiques par champ — un autre chantier, qui doit
// etre MESURE separement et pas glisse ici sans temoin.
//
// Cout par defaut : NUL. `g_any_hosted` est faux tant qu'aucune coroutine n'est
// hebergee, et le chemin pris est alors identique a l'octet a l'ancien.
static std::shared_mutex g_ev_mx;
static inline GEvent* ev_find(uint32_t h) {
    if (__builtin_expect(!g_any_hosted, 1)) {
        auto it = g_events.find(h);
        return it == g_events.end() ? nullptr : &it->second;
    }
    std::shared_lock<std::shared_mutex> lk(g_ev_mx);
    auto it = g_events.find(h);
    return it == g_events.end() ? nullptr : &it->second;
}
// `-1` = poignee inconnue. Comme ci-dessus : c'est la RECHERCHE qui est
// protegee, l'index rendu reste valide parce que `g_threads` ne retrecit jamais.
static inline int th_find(uint32_t h) {
    if (__builtin_expect(!g_any_hosted, 1)) {
        auto it = g_thread_handle.find(h);
        return it == g_thread_handle.end() ? -1 : it->second;
    }
    std::shared_lock<std::shared_mutex> lk(g_ev_mx);
    auto it = g_thread_handle.find(h);
    return it == g_thread_handle.end() ? -1 : it->second;
}

// PE static TLS (__declspec(thread)): the game's per-thread block — including
// the cached-tid struct read via fs:[0x2c] by the "am I thread [0x2925588]?"
// gates (0x80df30/0x502ff0) — is a loader-allocated copy of the .tls section,
// NOT TlsAlloc'd. Emulate the loader: index 0, template copied per thread.
#if defined(BO2_TARGET_SPS)
// Campagne Steam : son propre répertoire TLS (0xb28120 : index 0x314fb64, modèle .tls 0x40de000, 0x1a5c octets). Avec
// les valeurs t6sp ci-dessous, chaque fil recevait 0x1a5c octets hors image et l'index allait dans un global Steam.
static const uint32_t PE_TLS_INDEX_ADDR = 0x314fb64u;
static const uint32_t PE_TLS_TEMPLATE   = 0x40de000u;
static const uint32_t PE_TLS_SIZE       = 0x1a5cu;
#elif defined(BO2_TARGET_MPS)
static const uint32_t PE_TLS_INDEX_ADDR = 0x3339d74u;   // multi Steam, répertoire 0xc74f60
static const uint32_t PE_TLS_TEMPLATE   = 0x42c8000u;
static const uint32_t PE_TLS_SIZE       = 0x1a60u;
#else
static const uint32_t PE_TLS_INDEX_ADDR = 0x3337df4u;   // Zombies Steam, répertoire 0xc72b60
static const uint32_t PE_TLS_TEMPLATE   = 0x42c6000u;
static const uint32_t PE_TLS_SIZE       = 0x1a60u;
#endif
extern "C" uint32_t win32_tls_bitmap_reserve0();       // fwd (defined w/ bitmap)

static void pe_tls_fill(uint32_t blk) {
    for (uint32_t i = 0; i < PE_TLS_SIZE; i += 4)
        win32_mem_write32(blk + i, win32_mem_read32(PE_TLS_TEMPLATE + i));
}
static uint32_t pe_tls_new_block() {
    uint32_t blk = win32_guest_alloc(PE_TLS_SIZE, 16);
    pe_tls_fill(blk);
    return blk;
}

extern "C" void win32_install_crash_handler();
extern "C" void win32_gsc_dump_ring();   // GSC opcode-ring dump (win32_runtime.cpp)
static void sched_init() {
    if (g_sched_init) return;
    g_sched_init = true;
    win32_install_crash_handler();   // arm SIGSEGV/SIGBUS crumb+GSC-ring dump
    GThread* m = new GThread(); m->hstack = nullptr; m->status = T_RUN; m->wait_h = 0;
    m->wake_token = false;
    m->tls_va = 0x7FFD8000u;   // main's TLS array (boot template TEB [0x2C])
    win32_mem_write32(PE_TLS_INDEX_ADDR, 0);                 // loader-assigned index
    // Main gets a COPY of the template, like every other thread. Using the
    // template in-place dirtied it (main cached GetCurrentThreadId at +0x60)
    // and later threads' copies then inherited main's cached tid — every
    // thread passed the "am I thread [0x2925588]?" gates again. sched_init
    // runs at the very first import (CRT startup), before any game TLS write,
    // so the template is pristine here and stays pristine forever.
    win32_mem_write32(m->tls_va + 0, pe_tls_new_block());
    win32_tls_bitmap_reserve0();                             // TlsAlloc must skip index 0
    // ⚠️ RESERVER AVANT TOUT FIL HOTE. push_back peut REALLOUER le vecteur ;
    // un fil heberge qui lit g_threads[idx] pendant cette reallocation lit de
    // la memoire liberee. Le jeu cree 24 threads au plus observe.
    g_threads.reserve(64);
    g_threads.push_back(m); g_cur = 0;
    g_threads_pub.store(g_threads.size(), std::memory_order_release);
    // 🎯 TEMOIN [HOSTTID]. Le proxy SchedCur ne se voit pas : il rend le meme
    // entier qu'avant tant que rien n'est heberge, donc une regression y serait
    // muette. Cette ligne dit dans quel monde tourne le run — et le jour ou une
    // coroutine sortira sur un vrai fil, c'est elle qui le dira aussi.
    {   // ⚠️ TEMOIN NEGATIF. La premiere tentative a produit ZERO hebergement en
        // SILENCE, parce que le seul print vivait DANS le `if`. Un selecteur arme
        // qui ne matche rien doit le DIRE : c'est toute la difference entre
        // « le multicoeur ne paie pas » et « le multicoeur n'a jamais tourne ».
        const char* spec = bo2_getenv("BO2_HOST_THREADS");
        fprintf(stderr, "[HOSTTID] proxy arme ; BO2_HOST_THREADS=%s ; "
                        "coroutines hebergees=%d (t_host_tid=%d sur le fil ordonnanceur)\n",
                (spec && *spec) ? spec : "(vide -> comportement d'aujourd'hui)",
                g_hosted_n, t_host_tid);
    }
}
// Yield: switch to the next runnable thread (round-robin). Returns when THIS
// thread is resumed. If nothing else is runnable, returns immediately.
static uint64_t g_tick = 0;   // virtual time: one tick per cooperative switch
// Real-time thread hint, set by the audio shim (see the note in coop_yield).
extern "C" int g_rt_tid = -1;      // guest thread index of the sound mixer
extern "C" int g_rt_hungry = 0;    // 1 while the device is short of samples
static int g_rt_streak = 0;        // 1 = it just took a priority turn
// Per-thread scheduler accounting. "The mixer stopped" has two very different
// explanations — it is BLOCKED on something, or it is runnable and simply
// never gets picked — and they need opposite fixes. Count the picks and expose
// the status so the [SNDLOOP] sampler can say which one it is.
extern "C" uint64_t g_pick_count[32] = {};
// ── 📊 QUI TIENT LE CŒUR, ET COMBIEN DE TEMPS ────────────────────────────────
// g_pick_count dit COMBIEN DE FOIS un thread a ete elu. Il ne dit pas ce qui
// decide la question la plus chere du port : « une image coute 500 ms ; sont-ils
// repartis entre les threads invites, ou concentres dans un seul ? » Si 90 % est
// dans le Serveur, rendre les vrais threads de Windows ne rendra rien ; si c'est
// reparti sur trois ou quatre, c'est le gros lot et les sept cœurs qui dorment
// valent un facteur.
//
// UN SEUL HORODATAGE SUFFIT, et c'est ce qui rend l'instrument gratuit : les
// coroutines ucontext s'excluent par construction — il y en a exactement une qui
// tourne. Donc « le temps ecoule depuis la derniere bascule » appartient sans
// ambiguite au thread sortant. Chaque bascule facture, puis reamorce.
//
// COUT : une lecture de CLOCK_UPTIME_RAW (commpage, ~20 ns) par bascule. A
// ~20 000 bascules/s mesurees, 0,4 ms par seconde, soit 0,04 %. C'est sous le
// bruit de n'importe quel bras A/B de ce depot.
extern "C" uint64_t g_hold_ns[32] = {};
static uint64_t g_last_switch_ns = 0;
// Android: a diagnostic only (the [FILS] report reads it), and the clock read on every switch showed in the main
// thread's profile on the S26. Off unless BO2_HOLD_NS=1.
static int g_hold_on = -1;
static inline void sched_charge(int prev) {
    if (__builtin_expect(g_hold_on < 0, 0)) g_hold_on = bo2_env_on("BO2_HOLD_NS") ? 1 : 0;
    if (__builtin_expect(g_hold_on == 0, 1)) return;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    // Le tout premier appel n'a rien a facturer : il amorce seulement l'horloge.
    if (g_last_switch_ns && prev >= 0 && prev < 32) g_hold_ns[prev] += now - g_last_switch_ns;
    g_last_switch_ns = now;
}
extern "C" int win32_thread_status(int tid) {
    if (tid < 0 || tid >= (int)sched_nthreads()) return -1;
    return (int)g_threads[tid]->status;
}

// 🎯 THE GSC BUILTIN WINDOW IS NOT INTERRUPTIBLE ON WINDOWS. Opcode 0x29 saves
// the VM's stack pointer, calls a native builtin, and then consumes a per-VM
// word ([edi*0x42a8 + 0x2f3b8e0], "a result was produced") to decide whether to
// pop one more slot. If a coroutine yields inside that window, ANOTHER coroutine
// can run a builtin and consume the first one's flag — measured: coroutine 0 set
// it to 0xffffffff, coroutine 12 ran `setdvar` (a VOID builtin) and its handler
// took the `sub esi,8` branch. One slot too many, and the operand stack walks
// under its own base from then on. That single race is upstream of every symptom
// the four MT guards work around.
//
// Windows serialises this; we cannot, because every guest coroutine shares one
// host thread and guest CRITICAL_SECTIONs are recursive mutexes — so a guest CS
// gives no mutual exclusion between coroutines at all (critical sections
// included). Suppressing the switch for the duration of the window
// is the narrow equivalent.
//
// ⚠️ WITH A SAFETY VALVE. If a builtin ever genuinely blocks, refusing to yield
// forever would spin instead of deadlocking-visibly, which is worse. After
// kGscYieldCap consecutive refusals the yield is allowed through and the event is
// printed — a bounded, loud failure rather than a silent hang.
extern "C" int g_gsc_noyield;          // set/cleared in win32_runtime.cpp
extern "C" uint64_t g_gsc_yield_blocked;
uint64_t g_gsc_yield_blocked = 0;
static const int kGscYieldCap = 64;
// The four words opcode 0x29's handler round-trips across a native builtin call.
// Addresses are the GSC-thread-0 block; that is where the collision was measured
// (both colliding coroutines ran with edi = 0), and swapping only block 0 keeps
// this to four loads and four stores per context switch.
//   +0x2f3b8d4  the operand-stack pointer slot
//   +0x2f3b8d8  esi saved across the builtin call
//   +0x2f3b8e0  "a result was produced"  <-- the one that was stolen
//   +0x2f3b8e4  args left for the handler to pop
// ⚠️ 2026-08-13 — "SWAPPING ONLY BLOCK 0" WAS A BOUNDED ASSUMPTION AND THE BOUND
// HAS BEEN CROSSED. The reasoning above ("both colliding coroutines ran with
// edi = 0") was true while only the SERVER VM was executing script. The VM
// addresses its per-instance state as `edi*0x42a8 + 0x2f3b8c8` (0x87fbb2), so
// these four globals are INSTANCE 0's slots and nothing more:
//
//     instance 0   0x2f3b8c8 + 0        -> 0x2f3b8d4/d8/e0/e4
//     instance 1   0x2f3b8c8 + 0x42a8   -> 0x2f3fb7c/b80/b88/b8c
//
// The CLIENT VM is instance 1 (Scr_VarInit 0x574d80 is called exactly twice,
// 0x80c09d `push 0` and 0x5f0103 `push 1`), and it starts running script when
// the level does — `clientscripts/angola.csc`. From that moment the client VM's
// four scratch words crossed every coroutine switch completely unprotected,
// which is the same race this fix exists to close, on the instance it did not
// cover. Cover both blocks: eight loads and eight stores per switch instead of
// four, still no scheduling change.
static const uint32_t kGscScratch[8] = {
    0x2f3b8d4u, 0x2f3b8d8u, 0x2f3b8e0u, 0x2f3b8e4u,           // instance 0 (server)
    0x2f3b8d4u + 0x42a8u, 0x2f3b8d8u + 0x42a8u,               // instance 1 (client)
    0x2f3b8e0u + 0x42a8u, 0x2f3b8e4u + 0x42a8u,
};
static constexpr int kGscScratchN = (int)(sizeof(kGscScratch) / sizeof(kGscScratch[0]));
// ❌ 2026-09-11 — OFF BY DEFAULT: THIS SWAP WAS THE VM LEAK. Windows never rewrites
// guest memory on a thread switch. 0x2f3b8d4 is not "the operand-stack pointer
// slot" but the VM's FUNCTION-FRAME pointer (sub_87df60 initialises it to the
// frame array 0x2f3b8e8); a snapshot taken before the VM existed restored 0, so
// the archive sub_87e1d0 read each frame's local count from guest ~0x20 and popped
// 0 for a frame that pushed 6 — +24 bytes per pass of sub_591fd0 until the cursor
// ran into the sound globals ("The sound globals have been overwritten"). Named
// by [FRAME-PTR] (runs/fp_1); A/B runs/tls0_1: 0 leaking passes, spawn reached.
// The race it was written for measured 0 of 219,545 switches (05dcfa1).
// BO2_GSC_TLS=1 re-arms it for A/B only; delete once spawn holds N/N without it.
static bool gsc_tls_on() {
    static const bool on = [] {
        const char* e = bo2_getenv("BO2_GSC_TLS");
        return e && atoi(e) != 0;           // OFF by default; BO2_GSC_TLS=1 opts in (A/B only)
    }();
    return on;
}
// 🎯 HOW OFTEN DOES A COROUTINE SWITCH LAND INSIDE A BUILTIN WINDOW?
// Opcode 0x29 parks the operand-stack pointer and a "a result was produced" word
// in the VM's per-instance block, calls a native builtin, then reads them back.
// A switch inside that window is the race kGscScratch exists to close, and the
// port's first GSC error of the run is exactly its signature: a builtin entered
// with fewer operands than its declared minimum. Count the switches that happen
// with a window open, per instance, so "the race happens N times" stops being an
// assumption. The flag word is 0x2f3b8e0 (+0x42a8 for the client instance).
uint64_t g_gsc_switch_total, g_gsc_switch_in_window[2];
static void gsc_scratch_save(GThread* t) {
    if (!t) return;
#if defined(__ANDROID__)
    // Android : le comptage ci-dessous (deux lectures invitees par bascule) n'est qu'un temoin ; il pesait 2,2 % du
    // fil ordonnanceur sur le S26. BO2_GSC_TLS=1 (le seul usage de ces compteurs) le remet.
    if (!gsc_tls_on()) return;
#endif
    ++g_gsc_switch_total;
    if (win32_mem_read32(0x2f3b8e0u))          ++g_gsc_switch_in_window[0];
    if (win32_mem_read32(0x2f3b8e0u + 0x42a8u)) ++g_gsc_switch_in_window[1];
    if (!gsc_tls_on()) return;
    for (int i = 0; i < kGscScratchN; i++) t->gsc_scratch[i] = win32_mem_read32(kGscScratch[i]);
    t->gsc_saved = true;
}
static void gsc_scratch_restore(GThread* t) {
    // ⚠️ Only restore what we actually saved. A coroutine that has never yielded
    // has garbage here, and writing it into the VM would invent the very
    // corruption this exists to prevent.
    if (!gsc_tls_on() || !t || !t->gsc_saved) return;
    for (int i = 0; i < kGscScratchN; i++) win32_mem_write32(kGscScratch[i], t->gsc_scratch[i]);
}

// ── 🎯 [HOSTYIELD] — LE TEMOIN DE LA RECULADE GRADUEE DES FILS HOTES ─────────
//
// AGENTS.md : « un bras non arme se lit "le levier ne paie pas" ». La reculade
// de `coop_yield()` est invisible de l'exterieur — elle ne change qu'un
// `nanosleep`. Sans compteur, on ne peut pas distinguer « la graduation
// fonctionne » de « la graduation est epuisee et tout dort 200 us », et c'est
// exactement l'erreur que ce temoin vient de rattraper (P609.1).
//
// Trois bandes + le nombre d'ATTENTES detectees + le plus long tour de boucle
// vu dans UNE attente. Un slot par coroutine hebergee : chaque fil hote n'ecrit
// que dans le sien, le rapporteur lit d'un autre fil, et ce sont des compteurs —
// une lecture demi-ecrite fausse un chiffre, elle ne casse rien.
struct HostYieldStat { uint64_t band_yield, band_20us, band_200us, waits, k_max; };
static HostYieldStat g_hy[32];
// Seuil « nouvelle attente ». Cinq fois le plus grand ecart INTRA-attente
// (200 us), donc aucune attente en cours ne peut etre coupee en deux par
// accident ; et assez court pour qu'un fil qui a vraiment travaille reparte de
// zero. Conservateur par construction : si le travail dure moins que ca, on ne
// remet rien a zero, donc on n'est jamais PIRE que la version sans remise a
// zero — on ne peut qu'ameliorer.
static const uint64_t kHostYieldNewWaitNs = 1000000ull;   // 1 ms

extern "C" void bo2_hostyield_report() {
    static uint64_t p_y[32], p_20[32], p_200[32], p_w[32];
    for (int t = 0; t < 32; t++) {
        HostYieldStat& h = g_hy[t];
        const uint64_t dy = h.band_yield - p_y[t], d20 = h.band_20us - p_20[t],
                       d200 = h.band_200us - p_200[t], dw = h.waits - p_w[t];
        p_y[t] = h.band_yield; p_20[t] = h.band_20us; p_200[t] = h.band_200us; p_w[t] = h.waits;
        if (!(dy | d20 | d200 | dw)) continue;
        const uint64_t tot = dy + d20 + d200;
        fprintf(stderr, "[HOSTYIELD] tid=%-2d sur cette fenetre : cessions=%llu  "
                "yield=%llu (%.0f%%)  20us=%llu (%.0f%%)  200us=%llu (%.0f%%)  "
                "attentes=%llu  tours max dans UNE attente=%llu\n",
                t, (unsigned long long)tot,
                (unsigned long long)dy,   tot ? dy   * 100.0 / (double)tot : 0.0,
                (unsigned long long)d20,  tot ? d20  * 100.0 / (double)tot : 0.0,
                (unsigned long long)d200, tot ? d200 * 100.0 / (double)tot : 0.0,
                (unsigned long long)dw, (unsigned long long)h.k_max);
    }
}

#if defined(__APPLE__)   // [FILS-CPU] et [A-COUP] lisent les fils par Mach ; sur Android, voir le #else plus bas
// ── 🎯 [FILS-CPU] (P795) — QUI CONSOMME LES COEURS, FIL PAR FIL ─────────────
// `sample` compte aussi un fil qui attend son tour (swtch_pri, semaphore) : p788 met 816
// echantillons sur 1 679 dans le yield de host_backoff sans dire si le coeur brulait. Ici,
// le temps user+system rendu par le noyau (THREAD_BASIC_INFO), en coeurs sur ~10 s.
// Noms : « bo2_ordonnanceur », « bo2_h<idx>_<proc> » (pthread_setname_np), « main » = le
// premier fil (AppKit, pompe Metal), sinon le nom donne par le systeme ou le port.
// Un seul appelant (le fil echantillonneur de remaining_dlls_impl.cpp, toutes les 2 s).
extern "C" void bo2_filscpu_report() {
    static uint64_t t_prev = 0;
    static uint32_t p_port[256]; static uint64_t p_ns[256]; static uint32_t p_n = 0;
    const uint64_t now = bo2_mono_ns();
    if (t_prev && now - t_prev < 10000000000ull) return;
    thread_act_array_t th; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
    struct Row { uint64_t d; char nm[48]; };
    std::vector<Row> rows; uint64_t tot = 0;
    uint32_t c_port[256]; uint64_t c_ns[256]; uint32_t c_n = 0;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
        if (c_n < 256 && thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS) {
            const uint64_t ns = (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000000ull
                              + (uint64_t)(bi.user_time.microseconds + bi.system_time.microseconds) * 1000ull;
            c_port[c_n] = th[i]; c_ns[c_n++] = ns;
            uint64_t d = 0;
            for (uint32_t k = 0; k < p_n; k++)
                if (p_port[k] == th[i]) { d = ns >= p_ns[k] ? ns - p_ns[k] : 0; break; }
            Row r{d, ""};
            pthread_t p = pthread_from_mach_thread_np(th[i]);
            if (!p || pthread_getname_np(p, r.nm, sizeof r.nm) != 0 || !r.nm[0])
                snprintf(r.nm, sizeof r.nm, i == 0 ? "main" : "port%u", (unsigned)th[i]);
            for (char* q = r.nm; *q; q++) if (*q == ' ' || *q == '=') *q = '_';
            tot += d; rows.push_back(r);
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_act_t));
    memcpy(p_port, c_port, c_n * sizeof *c_port); memcpy(p_ns, c_ns, c_n * sizeof *c_ns); p_n = c_n;
    const uint64_t tp = t_prev; t_prev = now;
    if (!tp) return;
    const double fen = (double)(now - tp) / 1e9;
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d > b.d; });
    char line[2048];
    int o = snprintf(line, sizeof line, "[FILS-CPU] fen=%.1fs coeurs=%.2f fils=%u |", fen, (double)tot / 1e9 / fen, n);
    for (size_t k = 0; k < rows.size() && k < 16 && o < (int)sizeof line - 80; k++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %s=%.2f", rows[k].nm, (double)rows[k].d / 1e9 / fen);
    fprintf(stderr, "%s\n", line);
}

// ── [A-COUP] BO2_ACOUP_MS=N (Z-NATIF-23, 23b) — LE VEILLEUR D'A-COUPS ───────────────────────────────────────
// Un fil a part : la pompe est elle-meme arretee pendant les a-coups en jeu (run 034724 : 3 detections sur 3 a la
// reprise), elle ne peut donc ni les voir a temps ni dire ou elle etait. Toutes les 5 ms le veilleur lit deux
// battements : les images du jeu (gfx_guest_frames : un Present du fil de rendu) et les passes de la
// pompe. Sans image depuis N ms, une fois par a-coup : jusqu'a trois releves a 15 ms d'ecart de chaque fil « bo2_* »
// et de la pompe, l'etat lu AVANT la suspension puis la pile hote. PC fixe dans un appel systeme = bloque, la pile
// nomme l'attente ; PC fixe en code utilisateur : etat R = pret mais sans coeur, etat W ou U = defaut de page ; PC qui
// bouge = au travail. A la reprise, sur tout l'a-coup : CPU par fil, CPU du processus dont la part sur coeurs P,
// frequence moyenne (cycles par seconde de CPU), temps des fils prets mais sans coeur, pages relues du disque.
// Rien n'est imprime ni symbolise avant la reprise : stderr est une des ressources soupconnees. Les piles se lisent
// fil SUSPENDU, avec des appels Mach seulement entre suspend et resume (le fil peut tenir le verrou de malloc ou de
// stdio) ; la memoire par mach_vm_read_overwrite, qui echoue proprement hors d'une pile.
extern "C" uint64_t gfx_guest_frames();                 // metal_backend.mm : Present du jeu
static std::atomic<uint64_t> g_pompe_passes{0};         // boucle de pompe, win32_run_main
static thread_act_t g_pompe_port = 0;
namespace {
enum { AC_MAXT = 96, AC_NF = 9, AC_NS = 3 };
struct AcReleve { uint32_t n; uint32_t port[AC_MAXT]; uint64_t ns[AC_MAXT]; uint64_t t, cpu, pcpu, pret, cyc, pageins; };
void ac_releve(AcReleve& r) {
    static const mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t v; mach_timebase_info(&v); return v; }();
    thread_act_array_t th; mach_msg_type_number_t tot = 0;
    r.n = 0;
    if (task_threads(mach_task_self(), &th, &tot) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < tot; i++) {
        thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
        if (r.n < AC_MAXT && thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS) {
            r.port[r.n] = th[i];
            r.ns[r.n++] = (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000000ull
                        + (uint64_t)(bi.user_time.microseconds + bi.system_time.microseconds) * 1000ull;
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, tot * sizeof(thread_act_t));
    rusage_info_v6 ru{}; proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t*)&ru);
    r.t = bo2_mono_ns();
    r.cpu = (ru.ri_user_time + ru.ri_system_time) * tb.numer / tb.denom;
    r.pcpu = (ru.ri_user_ptime + ru.ri_system_ptime) * tb.numer / tb.denom;
    r.pret = ru.ri_runnable_time * tb.numer / tb.denom;   // fils prets, sur coeur OU non : sans coeur = pret - cpu
    r.cyc = ru.ri_cycles; r.pageins = ru.ri_pageins;
}
void* acoup_veilleur(void* arg) {
    const uint64_t seuil_ns = (uint64_t)(intptr_t)arg * 1000000ull;
    pthread_setname_np("veilleur_acoup");               // pas « bo2_ » : il ne se releve pas lui-meme
    fprintf(stderr, "[A-COUP] arme : seuil %d ms, fil veilleur\n", (int)(intptr_t)arg);
    static AcReleve base, fin;
    static char nm[AC_MAXT][40], etat[AC_NS][AC_MAXT]; static bool suivi[AC_MAXT];   // suivi : la pompe et les « bo2_* »
    static uint64_t pcs[AC_NS][AC_MAXT][AC_NF]; static uint8_t npc[AC_NS][AC_MAXT];
    uint64_t vu = gfx_guest_frames(), t_vu = bo2_mono_ns(), pv = g_pompe_passes.load(), t_pv = t_vu;
    bool dit = false; base.t = 0;
    for (;;) {
        usleep(5000);
        uint64_t now = bo2_mono_ns();
        if (const uint64_t p = g_pompe_passes.load(); p != pv) { pv = p; t_pv = now; }
        if (const uint64_t f = gfx_guest_frames(); f != vu) { vu = f; t_vu = now; dit = false; ac_releve(base); continue; }
        if (dit || !base.t || now - t_vu < seuil_ns) continue;
        dit = true;
        const double sans_image = (double)(now - t_vu) / 1e6, sans_pompe = (double)(now - t_pv) / 1e6;
        thread_act_array_t th; mach_msg_type_number_t tot = 0;
        if (task_threads(mach_task_self(), &th, &tot) != KERN_SUCCESS) continue;
        const uint32_t n = tot < AC_MAXT ? tot : AC_MAXT;
        for (uint32_t i = 0; i < n; i++) {
            pthread_t p = pthread_from_mach_thread_np(th[i]);
            if (th[i] == g_pompe_port) snprintf(nm[i], sizeof nm[i], "pompe");
            else if (!p || pthread_getname_np(p, nm[i], sizeof nm[i]) != 0) nm[i][0] = 0;
            for (char* q = nm[i]; *q; q++) if (*q == ' ' || *q == '=') *q = '_';
            suivi[i] = th[i] == g_pompe_port || strncmp(nm[i], "bo2_", 4) == 0;
        }
        int releves = 0; double t_rel[AC_NS];
        for (int s = 0; s < AC_NS; s++) {
            if (s) { usleep(15000); if (gfx_guest_frames() != vu) break; }
            t_rel[s] = (double)(bo2_mono_ns() - t_vu) / 1e6; releves = s + 1;
            for (uint32_t i = 0; i < n; i++) {
                npc[s][i] = 0; etat[s][i] = '?';
                if (!suivi[i]) continue;
                thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
                if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS
                    && bi.run_state >= 1 && bi.run_state <= 5) etat[s][i] = "RSWUH"[bi.run_state - 1];
                if (thread_suspend(th[i]) != KERN_SUCCESS) continue;
                arm_thread_state64_t st; c = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &c) == KERN_SUCCESS) {
                    uint64_t fp = arm_thread_state64_get_fp(st);
                    pcs[s][i][npc[s][i]++] = arm_thread_state64_get_pc(st);
                    pcs[s][i][npc[s][i]++] = arm_thread_state64_get_lr(st);
                    while (npc[s][i] < AC_NF && fp && !(fp & 7)) {
                        uint64_t w[2]; mach_vm_size_t lu = 0;
                        if (mach_vm_read_overwrite(mach_task_self(), fp, sizeof w, (mach_vm_address_t)w, &lu) != KERN_SUCCESS
                            || lu != sizeof w) break;
                        pcs[s][i][npc[s][i]++] = w[1] & 0x00007fffffffffffull;
                        if (w[0] <= fp) break;
                        fp = w[0];
                    }
                }
                thread_resume(th[i]);
            }
        }
        while (gfx_guest_frames() == vu && bo2_mono_ns() - t_vu < 2000000000ull) usleep(2000);   // la reprise, 2 s au plus
        const bool repris = gfx_guest_frames() != vu;
        ac_releve(fin);
        fprintf(stderr, "[A-COUP] %.0f ms sans image du jeu ; derniere passe de la pompe il y a %.0f ms ; %d releve(s)\n",
                sans_image, sans_pompe, releves);
        for (uint32_t i = 0; i < n; i++)
            for (int s = 0; s < releves; s++) {
                if (!npc[s][i]) continue;
                char line[1024];
                int o = snprintf(line, sizeof line, "[A-COUP-FIL] +%.0f ms %s etat=%c :", t_rel[s], nm[i], etat[s][i]);
                for (int k = 0; k < npc[s][i] && o < (int)sizeof line - 80; k++) {
                    Dl_info di{};
                    if (dladdr((void*)pcs[s][i][k], &di) && di.dli_sname)
                        o += snprintf(line + o, sizeof line - (size_t)o, " %.40s+0x%llx", di.dli_sname,
                                      (unsigned long long)(pcs[s][i][k] - (uint64_t)di.dli_saddr));
                    else o += snprintf(line + o, sizeof line - (size_t)o, " 0x%llx", (unsigned long long)pcs[s][i][k]);
                }
                fprintf(stderr, "%s\n", line);
            }
        const double cpu = (double)(fin.cpu - base.cpu) / 1e6;
        char line[2048];
        int o = snprintf(line, sizeof line, "[A-COUP] fin : %s%.0f ms sans image ; processus cpu=%.0f ms dont coeurs P=%.0f ms, %.2f GHz,"
                         " prets sans coeur=%.0f ms, pages relues=%llu ; par fil, ms :", repris ? "" : "pas de reprise, ",
                         (double)(fin.t - base.t) / 1e6, cpu, (double)(fin.pcpu - base.pcpu) / 1e6,
                         cpu > 0 ? (double)(fin.cyc - base.cyc) / (cpu * 1e6) : 0.0, (double)(fin.pret - base.pret) / 1e6 - cpu,
                         (unsigned long long)(fin.pageins - base.pageins));
        for (uint32_t k = 0; k < fin.n && o < (int)sizeof line - 80; k++) {
            uint64_t d = 0; const char* nom = ""; bool s = false;
            for (uint32_t j = 0; j < base.n; j++) if (base.port[j] == fin.port[k]) { d = fin.ns[k] - base.ns[j]; break; }
            for (uint32_t j = 0; j < n; j++) if (th[j] == fin.port[k]) { nom = nm[j]; s = suivi[j]; break; }
            if (!s && d < 1000000ull) continue;
            if (nom[0]) o += snprintf(line + o, sizeof line - (size_t)o, " %s=%.1f", nom, (double)d / 1e6);
            else o += snprintf(line + o, sizeof line - (size_t)o, " port%u=%.1f", (unsigned)fin.port[k], (double)d / 1e6);
        }
        fprintf(stderr, "%s\n", line);
        for (mach_msg_type_number_t i = 0; i < tot; i++) mach_port_deallocate(mach_task_self(), th[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)th, tot * sizeof(thread_act_t));
        base = fin; vu = gfx_guest_frames(); t_vu = bo2_mono_ns();
        dit = !repris;                                      // rearme : un a-coup qui suit aussitot est releve aussi
    }
    return nullptr;
}
}   // namespace
#else   // Android : pas de Mach (task_threads, thread_suspend, mach_vm_read). Les deux temoins se taisent, et le disent.
extern "C" void bo2_filscpu_report() {
    static bool dit = false;
    if (!dit) { dit = true; fprintf(stderr, "[FILS-CPU] indisponible sur Android (lit les fils par Mach)\n"); }
}
static std::atomic<uint64_t> g_pompe_passes{0};         // boucle de pompe, win32_run_main
namespace {
void* acoup_veilleur(void*) {
    fprintf(stderr, "[A-COUP] BO2_ACOUP_MS ignore : indisponible sur Android (suspend les fils par Mach)\n");
    return nullptr;
}
}   // namespace
#endif

// ── 🎯 LA RECULADE GRADUEE D'UN FIL HOTE ────────────────────────────────────
//
// 🔴 P628/E6 — EXTRAITE DE `coop_yield` LE 2026-09-21, sans changer une
// instruction de son mecanisme. Motif : elle a maintenant DEUX appelants qui
// n'ont rien a voir l'un avec l'autre — la cession d'un fil hote (ci-dessous)
// et le cul-de-sac du tourniquet (`:1226`), auquel s'ajoute E7 (l'echec de
// `TryEnterCriticalSection` sur un fil hote). Dupliquer une graduation dont
// P609.1 a deja montre qu'un detail de remise a zero la retourne entierement,
// c'etait se garantir trois versions divergentes.
//
// `stat_slot` : le slot de `g_hy` a crediter. C'est `t_host_tid` pour un fil
// heberge ; pour le fil ORDONNANCEUR en cul-de-sac c'est `g_cur`.
// ⚠️ NE PAS poser `t_host_tid = 0` sur le fil ordonnanceur pour « simplifier » :
//    en mode hybride ce fil execute encore d'autres coroutines par swapcontext,
//    et `g_cur` doit continuer de rendre `g_sched_cur` (voir le proxy SchedCur).
static inline void host_backoff(int stat_slot) {
        // ⚠️ UN FIL HOTE QUI ATTEND N'A PLUS PERSONNE A QUI RENDRE LA MAIN.
        // Les primitives bloquantes de l'invite (WFSO, Sleep) sont des boucles
        // `while (!signale) coop_yield();`. Sur le tourniquet, chaque tour rendait
        // le coeur a une AUTRE coroutine ; ici un `yield` pur brule un coeur
        // ENTIER — un huitieme de cette machine, vole au jeu, donc directement a
        // l'objectif FPS. Mesurer le parallelisme avec ca, ce serait mesurer
        // notre attente.
        // 🔴 LA PREMIERE VERSION AVAIT LE RAPPORT A L'ENVERS — mesure P602,
        // 2026-09-20. « 255 yield puis 50 us » passe **92 % du temps a ceder** :
        // sur macOS, `std::this_thread::yield()` revient en ~2,4 us des que
        // personne d'autre n'est pret, donc 255 cessions coutent ~600 us de coeur
        // BRULE pour 50 us de sommeil. Les quatre ouvriers JQ heberges ont ainsi
        // fait 237 391 866 `WaitForSingleObject` dans UNE fenetre de gel (82,6 %
        // des imports), contre 1 820 au temoin : ce ne sont pas des ouvriers qui
        // travaillent, c'est notre propre attente qui tourne a vide sur quatre
        // coeurs reels — et qui affame le jeu au passage en martelant `CsLock`.
        //
        // Reculade GRADUEE. Ceder garde un sens court : rendre le coeur a un fil
        // DEJA pret (typiquement le detenteur d'une section critique qu'on
        // attend). Au-dela, plus personne n'est pret et il faut dormir.
        //   tours 1-16      : `yield`      (passation immediate)
        //   tours 17-256    : 20 us        (verrou tenu un peu plus longtemps)
        //   au-dela         : 200 us       (vraie attente : evenement, file vide)
        // Plafond : 200 us reste tres en dessous de la periode du son (5,3 ms).
        // 🔴 P609.1 — LE COMPTEUR ETAIT A L'ECHELLE DU FIL, PAS DE L'ATTENTE
        // (trouve hors ligne 2026-09-20, en lisant ce bloc). `spins` etait
        // `thread_local` et n'etait remis a zero NULLE PART — les deux seules
        // occurrences de l'identifiant dans le fichier etaient sa declaration et
        // son increment. C'etait donc un compteur de DUREE DE VIE DU FIL, pas de
        // duree de l'ATTENTE, et le commentaire juste au-dessus decrivait un
        // comportement que le code n'avait pas. Des le 257e `coop_yield()` cumule
        // d'un fil heberge — atteint en quelques millisecondes — TOUTE cession
        // ulterieure dormait 200 us, quelle que soit l'attente. Le seul cas que ce
        // meme commentaire dit « garder un sens » (rendre le coeur a un fil DEJA
        // pret, typiquement le detenteur d'une section critique qu'on attend) etait
        // precisement celui qui ne se produisait plus jamais.
        //
        // LE DISCRIMINANT EST L'ECART ENTRE DEUX APPELS. A l'interieur d'une
        // attente les appels sont espaces de 2,4 us (yield), 20 us ou 200 us
        // (sommeil) ; entre DEUX attentes le fil a execute du code invite. Plus
        // d'1 ms d'ecart = nouvelle attente. Aucun appelant a modifier : les douze
        // sites de cession en beneficient du meme coup.
        thread_local uint64_t spins = 0, last_ns = 0;
        HostYieldStat& hy = g_hy[(stat_slot >= 0 && stat_slot < 32) ? stat_slot : 31];
        const uint64_t now_ns = bo2_mono_ns();
        if (now_ns - last_ns > kHostYieldNewWaitNs) { spins = 0; ++hy.waits; }
        last_ns = now_ns;
        const uint64_t k = ++spins;
        if (k > hy.k_max) hy.k_max = k;
        if (k <= 16) {
            ++hy.band_yield;
            std::this_thread::yield();
        } else {
            struct timespec ts = {0, (k <= 256) ? 20000 : 200000};
            if (k <= 256) ++hy.band_20us; else ++hy.band_200us;
            nanosleep(&ts, nullptr);
        }
}

// 🎯 C19 (2026-09-26) — LE CHANGEMENT DE CONTEXTE DU TOURNIQUET SANS swapcontext.
// Profil p662 101734 (srvbe) : le fil ordonnanceur passe 6,5 % de son temps DANS
// swapcontext, et rien de ce temps n'est du changement de pile : sigprocmask (x2, un
// par getcontext, un par setcontext), puis __sigaltstack et __getrlimit que
// getcontext appelle pour remplir uc_stack (populate_signal_stack_context). Arbre
// d'appel : coop_yield <- impl_Sleep, l'attente de groupe de taches 0x5cb6c0 qui
// fait Sleep(0) entre deux taches aidees.
// Ce que swapcontext preservait en plus de _setjmp, et pourquoi on peut s'en passer :
//  - le masque de signaux : personne ne le change dans le runtime (grep sigprocmask /
//    pthread_sigmask : 0), toutes les coroutines d'un fil partagent donc le meme ;
//  - FPCR (arrondi) : jamais ecrit (grep fesetround / fesetenv / fpcr : 0) ;
//  - uc_stack : la pile d'une coroutine qui a deja tourne est dans sp, sauve par _setjmp.
// `ctx` reste donc pour la premiere entree seulement : setcontext sur la pile neuve de
// makecontext (un sigprocmask par vie de coroutine, pas par tour). Les fils heberges ne
// passent jamais ici (host_thread_entry appelle thread_trampoline directement).
static inline void coop_switch(GThread* from, GThread* to) {
    from->sw_live = true;
    if (_setjmp(from->sw_jb) == 0) {
        if (to->sw_live) _longjmp(to->sw_jb, 1);
        setcontext(&to->ctx);
    }
}

static void coop_yield() {
    // 🎯 UN FIL HEBERGE N'A PAS DE TOURNIQUET. `swapcontext` ici sauterait dans
    // la pile d'une coroutine que le FIL ORDONNANCEUR est peut-etre en train
    // d'executer — deux fils dans le meme contexte, corruption immediate. Un fil
    // hote rend la main au systeme, c'est tout.
    if (__builtin_expect(g_any_hosted, 0) && k32_hors_ordonnanceur() && t_host_tid >= 0) {
        host_backoff(t_host_tid);
        return;
    }
    static const bool atomic = [] {
        const char* e = bo2_getenv("BO2_GSC_ATOMIC");
        return e && atoi(e) != 0;           // OFF by default — superseded by BO2_GSC_TLS
    }();
    if (atomic && g_gsc_noyield) {
        static int run = 0;
        if (++run <= kGscYieldCap) { g_gsc_yield_blocked++; return; }
        static int shouted = 0;
        if (shouted++ < 8)
            fprintf(stderr, "[GSC-ATOMIC] %d consecutive yields refused inside a builtin "
                            "window — letting this one through (a builtin is blocking; "
                            "the window is not supposed to)\n", kGscYieldCap);
        run = 0;
    } else {
        static int reset = 0; (void)reset;
    }
    sched_init();
    int n = (int)sched_nthreads();
    // Expire finite-timeout waits. A thread that blocks with a timeout is OUT of
    // the rotation, so nothing could ever advance its own spin counter — without
    // this it waits forever no matter what timeout it asked for. (That is exactly
    // how main parked: WFSO(0x1001e, 1) on an event nobody signals, while the
    // render backend spun waiting for main to release it.) Windows would return
    // WAIT_TIMEOUT; we do the same, in virtual time.
    g_tick++;
    for (int i = 0; i < n; i++) {
        GThread* t = g_threads[i];
        if (t->status == T_BLOCKED && t->wake_at && g_tick >= t->wake_at) {
            t->status = T_RUN; t->wake_at = 0;
        }
    }
    // ONE REAL-TIME THREAD GETS EVERY OTHER TURN. The Sound Mix loop must run
    // 187.5 times a second (one 6144-byte buffer = 5.333 ms of audio) and it
    // yields ~4 times per iteration — every one of those yields is legitimate,
    // it signals events other threads are genuinely waiting on. But under plain
    // round-robin each yield costs a FULL rotation, and a rotation is as slow as
    // the slowest thread in it (a whole render frame), which pinned the mixer at
    // ~30 iterations/s no matter what else was fixed.
    //
    // On Windows this thread runs concurrently on its own core; a cooperative
    // scheduler can only approximate that with priority. So: when the audio is
    // short of buffered samples, come back to it after ONE other thread has run
    // rather than after all of them. The streak counter is what keeps it an
    // approximation and not a monopoly — it can never take two turns in a row,
    // so every other thread still advances at half its former rate rather than
    // starving. g_rt_tid stays -1 until the mixer actually runs, so a build with
    // no sound behaves exactly as before.
    if (g_rt_tid >= 0 && g_rt_tid < n && g_rt_tid != g_cur && g_rt_hungry &&
        g_rt_streak == 0 && g_threads[g_rt_tid]->status == T_RUN &&
        !g_threads[g_rt_tid]->hosted) {
        g_rt_streak = 1;
        int prev = g_cur; g_cur = g_rt_tid;
        if (g_rt_tid < 32) g_pick_count[g_rt_tid]++;
        gsc_scratch_save(g_threads[prev]);
        sched_charge(prev);
        coop_switch(g_threads[prev], g_threads[g_rt_tid]);
        gsc_scratch_restore(g_threads[g_cur]);
        return;
    }
    // 🎯 BO2_RR_FAIR=1 — THE ROUND ROBIN KEEPS ITS OWN CURSOR.
    // It used to restart from the CURRENT thread. With the real-time (audio)
    // thread jumping the queue whenever it is hungry, that turned into a two-thread
    // ping-pong: from the rt thread (tid 10) the next runnable is tid 12; when tid
    // 12 yields, the rt rule sends control back to tid 10; and so on. Measured
    // (runs/ng_coop4): tid 10 and tid 12 picked 42,214,266 times between two
    // reports while tids 0..9 stayed at the same count — tid 3 (Database), holding
    // a critical section tid 10 waits for, NEVER RAN AGAIN. Windows' scheduler does
    // not starve a runnable thread indefinitely; a cursor that only the round robin
    // advances gives every runnable coroutine its turn whatever the rt thread does.
    static const bool rr_fair = [] { const char* e = bo2_getenv("BO2_RR_FAIR"); return e && atoi(e) != 0; }();
    static int rr_cursor = 0;
    const int rr_from = rr_fair ? rr_cursor : g_cur;
    for (int off = 1; off <= n; off++) {
        int i = (rr_from + off) % n;
        if (g_threads[i]->status == T_RUN && !g_threads[i]->hosted) {
            if (rr_fair) rr_cursor = i;
            // 🔴 P628/E6 RETIRE LE 2026-09-21 10:05, PAR LA MESURE, EN UN RUN.
            // Il posait ici `if (g_any_hosted) host_backoff(g_cur);` — une
            // reculade graduee allant jusqu'a 200 us de sommeil. Resultat mesure
            // (`runs/p628_valid`, bras jq,snd,snddec) : gel a svsTime=41 750 sans
            // jamais charger angola, la ou les runs HEBERGES de P615, sans ce
            // correctif, atteignaient 170 000-187 000 avec angola charge 3/3.
            // Le correctif etait donc STRICTEMENT PIRE que le defaut qu'il visait.
            //
            // 🎯 LE MECANISME, et c'est ma propre memoire `scheduler-ticks-are-not-time`
            // appliquee A L'ENVERS. Le tourniquet est la BASE DE TEMPS des
            // attentes cooperatives : dans `impl_WaitForSingleObject`, un delai
            // fini expire sur `spins >= timeout`, ou `spins` s'incremente UNE
            // FOIS PAR TOUR. Faire dormir le tour 200 us transforme un
            // `WFSO(h, 100 ms)` en **20 secondes**. Le meme raisonnement vaut
            // pour `g_tick`, qui pilote la passe de reveil des delais finis.
            // J'avais deja ecrit que « un delai compte en TOURS cesse d'exister
            // des qu'un fil quitte le tourniquet » ; je n'avais pas vu la
            // reciproque : **changer la DUREE d'un tour change tous les delais
            // qui se comptent en tours.**
            //
            // ⚠️ ET LE CORRECTIF NE POUVAIT MEME PAS FAIRE SON TRAVAIL. Un fil
            // HOTE n'atteint jamais ce tourniquet : `coop_yield` le detourne vers
            // `host_backoff` des sa premiere ligne. Ce code ne s'executait donc
            // QUE sur le fil ordonnanceur — c'est-a-dire uniquement le mal, jamais
            // le bien vise. Sa premisse (« le fil du jeu n'est pas heberge, donc
            // le tourniquet est vide ») venait de `all`, qui est abandonne : avec
            // jq,snd,snddec les coroutines du jeu, du serveur et du backend
            // restent dans le tourniquet, qui trouve donc presque toujours
            // quelqu'un. E6 soignait un mal que la configuration retenue n'a pas.
            if (i == g_cur) return;                  // only ourselves runnable: carry on
            int prev = g_cur; g_cur = i;
            g_rt_streak = 0;
            if (i < 32) g_pick_count[i]++;
            gsc_scratch_save(g_threads[prev]);
            sched_charge(prev);
            coop_switch(g_threads[prev], g_threads[i]);
            gsc_scratch_restore(g_threads[g_cur]);   // we are back: reclaim ours
            return;
        }
    }
    // (P628/E6 posait ici la meme reculade qu'au cul-de-sac ci-dessus. Retiree
    //  pour la meme raison, et elle etait PIRE ici : ce cul-de-sac est celui d'un
    //  appelant deja T_BLOCKED, donc precisement celui qui attend l'expiration
    //  d'un delai compte en tours.)
}
int g_cs_depth[64] = {0};
// BO2_CS_COOP=1 — les sections critiques invitees excluent vraiment les autres
// coroutines (voir impl_EnterCriticalSection). Un seul lieu de lecture : la
// barriere d'ecriture en a besoin bien avant le code des sections critiques, et
// la lambda statique etait deja dupliquee dans Enter et TryEnter.
static inline bool cs_coop_on() {
    static const bool on = [] {
        const char* e = bo2_getenv("BO2_CS_COOP"); return e && atoi(e) != 0;
    }();
    return on;
}
uint64_t g_imp_under_lock = 0, g_imp_total = 0;
extern "C" int  win32_cs_depth_of(int tid) { return (tid >= 0 && tid < 64) ? g_cs_depth[tid] : -1; }
extern "C" void win32_cs_depth_counts(unsigned long long* under, unsigned long long* tot) {
    *under = g_imp_under_lock; *tot = g_imp_total;
}
uint64_t g_preempt_ticks[64] = {}, g_preempt_fired[64] = {},
         g_preempt_refused_lock[64] = {}, g_preempt_refused_cb[64] = {};
// Compteurs de la preemption COMPTEE SUR LES ECRITURES (BO2_PREEMPT_W), tenus a
// part de ceux des imports : les deux mecanismes doivent rester lisibles
// separement, sinon « fired » ne dit plus lequel a tire.
uint64_t g_preempt_w_fired[64] = {}, g_preempt_w_rlock[64] = {},
         g_preempt_w_rcb[64] = {},
         g_preempt_w_ulock[64] = {};
extern "C" void win32_preempt_counts(int tid, unsigned long long* ticks,
                                     unsigned long long* fired,
                                     unsigned long long* rlock,
                                     unsigned long long* rcb) {
    const int t = (tid >= 0 && tid < 64) ? tid : 0;
    *ticks = g_preempt_ticks[t]; *fired = g_preempt_fired[t];
    *rlock = g_preempt_refused_lock[t]; *rcb = g_preempt_refused_cb[t];
}
// ── 🎯 BO2_PREEMPT=N — GIVE THE COOPERATIVE SCHEDULER A TIMESLICE ───────────
//
// THE DEFECT THIS TESTS. Every guest coroutine runs on ONE host thread via
// swapcontext, and the scheduler NEVER preempts (see the note at
// win32_runtime.cpp: "scheduler never preempts mid-shim, so a plain RMW is
// genuinely atomic here"). coop_yield() is reachable from only eight sites, all
// of them BLOCKING primitives. So a guest thread that merely COMPUTES — even one
// making millions of EnterCriticalSection calls, none of which block — holds the
// host thread indefinitely and starves every other guest thread. On Windows
// these are preemptive OS threads and this cannot happen.
//
// MEASURED, 13 runs: at the terminal freeze exactly one coroutine is running,
// and which one VARIES — Server 7/13, Sound Mix 3/13, Backend 1, Main 1. That is
// why every candidate CAUSE found over months is a minority event (storm 4/7,
// spin 1/7, DIV0 2/7, MagicBullet 3/7, MT cycles 2/11) while the SYMPTOM is
// 11/11: different causes, one mechanism.
//
// PRE-REGISTERED PREDICTION: if the freeze is starvation-by-non-yielding, a run
// that currently reaches fps=0 should keep producing frames with this armed. If
// frames still stop, starvation is NOT the mechanism and this is refuted in one
// run.
//
// ⚠️ OFF BY DEFAULT, AND HERE IS THE HAZARD. Guest critical sections are
// PTHREAD_MUTEX_RECURSIVE on the shared host thread, so if coroutine A holds one
// and we yield to coroutine B, B's lock of the SAME object SUCCEEDS instead of
// blocking — mutual exclusion between coroutines is not enforced by that mutex.
// Today that is masked because the scheduler only yields at blocking primitives.
// Adding yield points widens the exposure, so this must be measured against the
// error-format core (tools/fmt_core.py), not just "it boots".
//
// ⚠️ The yield happens at the TOP of win32_dispatch_import, BEFORE the shim runs,
// so no shim state is half-updated — the guest is at a call boundary, which is
// the same place Windows would preempt it.
static void coop_yield();
extern "C" int g_in_shim_callback;
// 🔴 P642/C — « ce fil-ci est dans un rappel de shim ». Le compte global dit
// seulement « QUELQU'UN y est » : heberge, un rappel sur le fil du son faisait
// refuser la preemption et le cede au fil ordonnanceur, et reciproquement. Le
// global reste la porte (cout inchange en cooperatif) ; le compte par fil n'est
// lu que s'il est non nul ET qu'une coroutine est hebergee.
extern thread_local int t_in_shim_cb;
static inline bool in_shim_cb() {
    return g_in_shim_callback && (!__builtin_expect(g_any_hosted, 0) || t_in_shim_cb);
}
extern uint64_t g_setorigin_completed;
extern "C" void bo2_phead_arm_tick();   // win32_runtime.cpp
extern "C" void bo2_puaf_arm_tick();    // win32_runtime.cpp
extern "C" void bo2_frameptr_poll();    // win32_runtime.cpp
extern "C" void win32_thread_import_tick() {
    bo2_phead_arm_tick();
    bo2_frameptr_poll();
    bo2_puaf_arm_tick();
    // ⚠️ COUNTED UNCONDITIONALLY, BEFORE THE EARLY-OUT. The question "what
    // fraction of import dispatches happen while this coroutine holds a guest
    // lock" must be answerable in the CONTROL arm too — that fraction IS the
    // probability that any given preemption lands inside a critical section, so
    // measuring it only when preemption is armed would beg the question.
    ++g_imp_total;
    if (g_cur >= 0 && g_cur < 64 && g_cs_depth[g_cur] > 0) ++g_imp_under_lock;
    // 🎯 PER-COROUTINE, AND THE FIRST ONE IS THE DECISIVE LINE.
    // This hook only fires on IMPORTS. A coroutine that computes without calling
    // one is never interrupted — so "how many preemptions reached the wedged
    // coroutine" separates two very different situations, and the answer picks
    // the next tool:
    //   ticks flat  -> it passes NO yield point at all; an import counter can
    //                  never reach it and the fix is a TIMER (a dedicated host
    //                  thread doing pthread_kill(guest, SIGURG); NOT setitimer,
    //                  which would signal any host thread including Metal and the
    //                  audio callback).
    //   fired > 0   -> preemption is being delivered and is not sufficient; the
    //                  remaining stall is a second mechanism and a timer buys
    //                  nothing.
    // Counted for every coroutine, armed or not, so the control arm answers it too.
    if (g_cur >= 0 && g_cur < 64) ++g_preempt_ticks[g_cur];
    static const int every = [] {
        const char* e = bo2_getenv("BO2_PREEMPT");
        return e ? atoi(e) : 0;
    }();
    if (__builtin_expect(every <= 0, 1)) return;
    // Optional angola experiment: preserve startup scheduling until the requested
    // number of setorigin builtins has RETURNED. This is an explicit diagnostic
    // milestone, not a general proof of gameplay or a default map-specific fix.
    static const int after_origin = [] {
        const char* e = bo2_getenv("BO2_PREEMPT_AFTER_ORIGIN");
        return e ? atoi(e) : 0;
    }();
    if (after_origin > 0) {
        if (g_setorigin_completed < (uint64_t)after_origin) return;
        static bool announced = false;
        if (!announced) {
            announced = true;
            fprintf(stderr, "[PREEMPT-ARM] after %llu completed setorigin calls; every=%d\n",
                    (unsigned long long)g_setorigin_completed, every);
        }
    }
    // The audio callback borrows the running coroutine's State and suppresses
    // SetEvent yields until it restores that State. Import-boundary preemption
    // must respect the same exclusion; otherwise another coroutine runs while
    // the shared callback guard and borrowed State are still active.
    if (in_shim_cb()) { if (g_cur >= 0 && g_cur < 64) ++g_preempt_refused_cb[g_cur]; return; }
    // ONE global counter, deliberately: all coroutines share this host thread, so
    // "every N imports" is a property of the thread, not of a guest thread. (A
    // thread_local here would be read from the host and shared by every
    // coroutine anyway — a trap this repo has already paid for.)
    // ⚠️ NEVER YIELD WHILE THIS COROUTINE HOLDS A GUEST CRITICAL SECTION.
    // Without this the preemption experiment is uninterpretable: a yield under a
    // lock is a GUARANTEED mutual-exclusion violation (recursive pthread mutex,
    // owner = host thread, shared by every coroutine), not a rare race. And the
    // exposure is not small — MEASURED at the freeze: 41.9% of import dispatches
    // happen while the running coroutine holds a lock (56.2% during load).
    //
    // ⚠️ AND THIS GUARD COULD HAVE MADE THE EXPERIMENT A NO-OP, so it was checked
    // rather than assumed: if the WEDGED coroutine held a lock, its depth would
    // never return to zero and preemption would never fire on the one thread that
    // matters. Measured at the terminal freeze: the wedged coroutine (tid 10,
    // "Sound Mix") holds ZERO. The guard fires exactly where it is needed.
    //
    // (Separately noted, not acted on: tid 0 "Main" holds depth=1 at the freeze.
    // That is harmless TODAY only because the recursive mutex lets anyone re-enter
    // it. A future coroutine-aware critical section would make that lock really
    // block — a deadlock the current bug is masking.)
    if (g_cur >= 0 && g_cur < 64 && g_cs_depth[g_cur] > 0) {
        ++g_preempt_refused_lock[g_cur]; return;
    }
    static uint32_t n = 0;
    if (++n < (uint32_t)every) return;
    n = 0;
    if (g_cur >= 0 && g_cur < 64) ++g_preempt_fired[g_cur];
    coop_yield();
}

// ── 🎯 BO2_PREEMPT_W=N — PREEMPTER UNE BOUCLE QUI N'APPELLE AUCUN IMPORT ─────
//
// POURQUOI CELLE-CI EN PLUS DE BO2_PREEMPT. Le recensement [PREEMPT] du bras de
// controle (runs/q2_fast, gel a l'image 2355) imprime, mot pour mot :
//
//   [PREEMPT] tid=0 imports=1152660(+0) fired=0 ...
//             <- MAKES NO IMPORTS: an import-counter preemption can never reach it
//
// La coroutine coincee ne franchit AUCUN point de cede : elle tourne dans
// sub_8fb170, la file sans verrou du pool de workers (tid 5..8, proc=0x8fb8e0),
// autour de `lock xadd dword ptr [edx], esi` en 0x8fb275. Le detecteur de spin
// existant (cas_spin_note) ne peut pas la voir : il ne reconnait qu'un spin qui
// N'ECRIT RIEN, et celui-ci ecrit a chaque tour.
//
// Mais il ECRIT — et la sonde [SPIN] l'a echantillonnee dans
// __remill_write_memory_32. Un compteur d'ECRITURES l'atteint donc, la ou un
// compteur d'imports ne le peut pas, et sans le danger d'un vrai timer : pas de
// signal, pas de pthread_kill, pas de commutation depuis un gestionnaire — on
// est dans un appel C ordinaire, pile hote coherente, State en memoire.
//
// ⚠️ MEMES EXCLUSIONS QUE LA PREEMPTION AUX IMPORTS, pour la meme raison : les
// sections critiques invitees sont des mutex recursifs appartenant au thread
// HOTE, donc ceder sous verrou n'est pas une course rare mais une violation
// garantie de l'exclusion mutuelle. Et le callback de shim emprunte le State de
// la coroutine courante.
extern "C" int bo2_hwa_defer_yield(int kind);   // win32_runtime.cpp (P645)
#if defined(__ANDROID__)
extern "C" volatile uint32_t bo2_preempt_now;   // win32_runtime.cpp: raised by the time-slice timer
#endif
extern "C" void win32_thread_write_tick() {
#if defined(__ANDROID__)
    // Callers test the flag and the scheduler thread first; lower it here, once per slice, whatever happens below
    // (a refusal waits for the next slice, as the write countdown's reset did).
    __atomic_store_n(&bo2_preempt_now, 0u, __ATOMIC_RELAXED);
#endif
    if (in_shim_cb()) { if (g_cur >= 0 && g_cur < 64) ++g_preempt_w_rcb[g_cur]; return; }
    const bool under_lock = (g_cur >= 0 && g_cur < 64 && g_cs_depth[g_cur] > 0);
    // Compte INCONDITIONNELLEMENT, comme g_imp_under_lock : « quelle fraction
    // des cedes tombe sous un verrou invite » doit rester lisible dans le bras
    // ou l'on ne refuse plus, sinon armer coop rendrait la question sans reponse.
    if (under_lock) ++g_preempt_w_ulock[g_cur];
    // ⚠️ CETTE GARDE N'EST NECESSAIRE QUE TANT QUE LES SECTIONS CRITIQUES
    // N'EXCLUENT RIEN. Sans BO2_CS_COOP le mutex hote est recursif et appartient
    // au thread hote que toutes les coroutines partagent : ceder sous un verrou
    // laisserait une autre coroutine entrer dans la meme section — violation
    // garantie, pas course rare. Avec BO2_CS_COOP=1, EnterCriticalSection retient
    // le tid INVITE proprietaire et cede jusqu'a sa liberation, donc la
    // concurrente attend : ceder sous un verrou redevient la semantique Windows.
    // Mesure sans lui (runs/pw20k) : 234 cedes contre 90 947 refus, et 100 % de
    // refus sur la coroutine coincee — la garde annulait l'experience.
    if (under_lock && !cs_coop_on()) { ++g_preempt_w_rlock[g_cur]; return; }
    // P645 : au milieu d'une instruction `lock`, le cede attend la sortie de la
    // sequence (win32_runtime.cpp, hwa_defer) ; il y repasse par ici.
    if (bo2_hwa_defer_yield(3)) return;
    if (g_cur >= 0 && g_cur < 64) ++g_preempt_w_fired[g_cur];
    coop_yield();
}
extern "C" void win32_preempt_w_counts(int tid, unsigned long long* fired,
                                       unsigned long long* rlock,
                                       unsigned long long* rcb,
                                       unsigned long long* ulock) {
    const int t = (tid >= 0 && tid < 64) ? tid : 0;
    *fired = g_preempt_w_fired[t]; *rlock = g_preempt_w_rlock[t];
    *rcb   = g_preempt_w_rcb[t];   *ulock = g_preempt_w_ulock[t];
}

// A shim that has to run guest code as a CALLBACK (the WNDPROC trampoline, the
// XAudio2 OnBufferEnd pump) borrows the current guest thread. Anything that
// yields inside such a callback costs a whole scheduler rotation for what the
// real Windows API would have done inline — and for OnBufferEnd, which the
// engine invokes once per finished audio buffer, that capped the whole mixer at
// one buffer per rotation (~30/s against the 187.5/s a 48 kHz device needs).
// Waiters woken during the callback still become runnable; they are simply
// picked up at the next natural scheduling point instead of immediately.
extern "C" int  g_in_shim_callback = 0;
thread_local int t_in_shim_cb = 0;        // P642/C
// Public yield, for a shim that must give the CPU back explicitly — e.g. the
// audio throttle predicate when it reports "no work": that loop has no other
// yield point of its own, so without this it would spin-hog the scheduler.
// WHERE DOES A THREAD GIVE UP ITS TURN? The Sound Mix loop was still managing
// exactly one iteration per scheduler rotation after SetEvent stopped yielding
// inside the audio callback, which means something ELSE in the iteration yields.
// Guessing which import that is has a bad track record on this project, so
// attribute every yield to (tid, reason) and read it off.
extern "C" uint64_t    g_yield_count[32] = {};
// The handle a thread last blocked on. "last=WFSO-event" says the mixer is
// waiting rather than starved of CPU; the handle says WHAT it waits for, which
// is the only way to find who is supposed to signal it.
extern "C" uint32_t    g_yield_handle[32] = {};
extern "C" const char* g_yield_last[32]  = {};
// Android — UN SONDEUR SANS PROGRES N'INTERDIT PAS LE REPOS. Sur le S26, au menu, le fil ordonnanceur restait a un
// cœur plein : des coroutines « RUN sans echeance » qui ne font que sonder (Sleep(0), WFSO non signale, TryCS)
// refusaient le repos des millions de fois ([ORDO-REPOS refus #67108864]). Une coroutine dont la cession est un
// sondage, et qui n'a ecrit aucun octet invite depuis son sondage precedent (son compte d'ecritures, State+2760,
// tenu par le code leve), n'a rien fait : apres deux sondages vides de suite, ordo_repos_si_inactif l'ignore. Tout
// ce qu'elle attend la reveille de toute facon (un SetEvent d'un fil heberge reveille l'ordonnanceur) et le repos
// est borne a 1 ms. BO2_ORDO_SONDE=0 rend l'ancien comportement.
static uint64_t g_poll_writes[32];
static uint32_t g_poll_idle[32];
static bool poll_reason(const char* why) {
    return !strcmp(why, "Sleep") || !strncmp(why, "WFSO", 4) || !strcmp(why, "TryCS") || !strcmp(why, "CS-wait");
}
static bool ordo_sonde_on() {
#if defined(__ANDROID__)
    static const bool on = bo2_env_on("BO2_ORDO_SONDE", true);
    return on;
#else
    return false;
#endif
}
static void coop_yield_from(const char* why) {
    if (g_cur >= 0 && g_cur < 32) {
        g_yield_count[g_cur]++; g_yield_last[g_cur] = why;
        if (ordo_sonde_on()) {
            const GThread* t = g_threads[g_cur];
            uint64_t w = 0;
            if (t && t->cur_state) memcpy(&w, reinterpret_cast<const uint8_t*>(t->cur_state) + 2760, sizeof w);
            if (poll_reason(why) && w == g_poll_writes[g_cur]) {
                if (g_poll_idle[g_cur] < 0xffffffffu) ++g_poll_idle[g_cur];
            } else {
                g_poll_idle[g_cur] = 0;
            }
            g_poll_writes[g_cur] = w;
        }
    }
    coop_yield();
}
// 🎯 ATTENDRE JUSQU'A UNE ECHEANCE, quand on la connait — P602, 2026-09-20.
// Sur le tourniquet, ceder est la seule chose a faire : le temps n'avance que
// parce qu'une AUTRE coroutine tourne. Sur un fil hote, on connait l'heure de
// fin, personne n'attend le coeur, et redemander l'echeance 64 fois par
// milliseconde ne la fait pas arriver plus vite — ca brule juste un coeur que le
// jeu reclame. On dort donc par tranches, assez courtes pour qu'un `SetEvent`
// reste vu vite (500 us << 5,3 ms, la periode du son), assez longues pour que
// l'attente ne coute rien.
static void coop_yield_until(const char* why, uint64_t deadline_ns) {
    if (deadline_ns && hosted_self()) {
        const uint64_t now = bo2_mono_ns();
        if (now < deadline_ns) {
            uint64_t reste = deadline_ns - now;
            if (reste > 500000ull) reste = 500000ull;       // tranche de 500 us max
            struct timespec ts = {0, (long)reste};
            nanosleep(&ts, nullptr);
        }
        if (g_cur >= 0 && g_cur < 32) { g_yield_count[g_cur]++; g_yield_last[g_cur] = why; }
        return;
    }
    coop_yield_from(why);
}

// 🎯 C25 (2026-09-26) — UN FIL HEBERGE BLOQUE SUR UN EVENEMENT SE REVEILLE QUAND ON LE SIGNALE.
// Sous Windows, SetEvent et PulseEvent liberent l'attendeur dans le noyau, tout de
// suite. Chez nous, un fil heberge dans WFSO dormait par tranches de nanosleep
// (500 us au plus). Il ne voyait le signal qu'a la fin de sa tranche, et un pulse
// tombe entre deux tranches (statut T_RUN) etait perdu jusqu'a l'echeance.
// Le cas qui compte : la file de taches. Main appelle PulseEvent(0x31fd604)
// (evenement a reset manuel, 0x8fa8a0) en tete de 0x8fb9c0, et les ouvriers jq
// dorment dans 0x8f9f20 sur WFSO(h, 1). Sample p670 161759 : Main passe 29 % dans
// l'attente de groupe 0x8fb5d0 et 18 % dans 0x8fb170, dont 20,5 % a executer
// lui-meme des taches, pendant que les 5 ouvriers dorment 35 % du temps.
// On garde la tranche de 500 us, qui reste le filet de securite de tout signaleur
// qui ne passe pas par ici ; une attente n'est donc jamais plus longue qu'avant.
// Android (S26, jq heberges) : la tranche de 500 us n'a plus de raison d'etre pour un evenement. SetEvent et
// PulseEvent reveillent tout attendeur heberge par son semaphore (host_wake, ci-dessous), et un semaphore compte :
// un signal donne avant l'attente n'est pas perdu. Le seul autre signaleur (BO2_ORPHAN_SIGNAL) est un diagnostic.
// Mesure (run hosted jq, 10:53) : ~14 700 parcages/s pour 5 ouvriers inactifs, 85 % finis par la tranche et non par
// un signal ; chaque reveil les tire d'un sommeil profond. L'ouvrier jq attend WFSO(evt, 1 ms) en boucle (0x9b0450,
// le pousseur fait PulseEvent a chaque tache) : on dort jusqu'a l'echeance, comme Windows apres timeBeginPeriod(1)
// que le jeu appelle. Sans echeance (attente infinie), la tranche passe a 2 ms. BO2_HOST_PARK_US=500 rend l'ancien
// comportement (plafond de tranche, en microsecondes ; 0 = pas de plafond pour une echeance finie).
static uint64_t host_park_cap_ns(bool fini) {
    static const long us = [] {
        const char* e = bo2_getenv("BO2_HOST_PARK_US");
        return e ? atol(e) : -1L;
    }();
    if (us > 0) return (uint64_t)us * 1000ull;
    return fini ? ~0ull : 2000000ull;
}
static std::atomic<uint64_t> g_c25_park{0}, g_c25_reveil{0};
static void host_park_until(GThread* me, uint64_t deadline_ns, const char* why) {
    const uint64_t now = bo2_mono_ns();
    uint64_t reste = (deadline_ns > now) ? deadline_ns - now : 0;
    const uint64_t cap = host_park_cap_ns(deadline_ns != 0);
    if (!deadline_ns || reste > cap) reste = deadline_ns ? cap : host_park_cap_ns(false);
    if (reste) {
        const long rc = dispatch_semaphore_wait(me->hsem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)reste));
        const uint64_t k = g_c25_park.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t r = (rc == 0) ? g_c25_reveil.fetch_add(1, std::memory_order_relaxed) + 1
                                     : g_c25_reveil.load(std::memory_order_relaxed);
        if (k >= 1024 && (k & (k - 1)) == 0)   // puissances de deux : #N est le VRAI compte
            fprintf(stderr, "[C25] attentes hebergees=%llu reveillees par signal=%llu (%s)\n",
                    (unsigned long long)k, (unsigned long long)r, why);
    }
    if (g_cur >= 0 && g_cur < 32) { g_yield_count[g_cur]++; g_yield_last[g_cur] = why; }
}
static inline void host_wake(GThread* t) {
    if (dispatch_semaphore_t sem = __atomic_load_n(&t->hsem, __ATOMIC_ACQUIRE))
        dispatch_semaphore_signal(sem);
}

// 🎯 P796 — LE TOURNIQUET DORT QUAND TOUTES SES COROUTINES DORMENT.
// [FILS-CPU] P795, com_maxfps 30 : le fil ordonnanceur brule 0,97 coeur. [SLEEP-REAL] : Main (tid 0,
// limiteur de Com_Frame) et WebM Decode (tid 13) font Sleep(1), et la boucle Sleep-real cede ~6 000
// fois par milliseconde de l'un a l'autre. Sous Windows, Sleep(1) bloque le fil et libere le coeur.
// Ici, quand aucune AUTRE coroutine du tourniquet ne peut avancer (chacune dort jusqu'a une echeance
// connue, ou attend un signal sans delai), le fil ordonnanceur dort jusqu'a la plus proche echeance,
// 1 ms au plus ; SetEvent/PulseEvent d'un fil heberge le reveillent (ordo_sem). Une attente a delai
// fini (wake_at) se re-teste a chaque tour : elle interdit le repos, sauf au niveau 2 (P800) ou un WFSO
// a delai fini sur un evenement publie son echeance (wfso_until_ns) : Main attend le backend en WFSO(h,1)
// (sub_502ff0) ~10 % du temps en combat (P798), et SetEvent reveille ordo_sem.
// BO2_ORDO_REPOS : 0 desarme, 1 Sleep/Present-sync (defaut), 2 + WFSO-event. Temoin : [ORDO-REPOS].
// Niveau 3 (Z-NATIF-14z, defaut sous Zombies) : + CS-wait. Au menu de Zombies, Main attend ~17 ms par image une
// section critique tenue par un autre fil ([CS-COOP] tid 0 waited 45 723 turns for section 0x02b151f8) : sous Windows
// EnterCriticalSection bloque le fil dans le noyau ; ici la coroutine cedait en boucle, comptait comme « peut
// avancer » et interdisait le repos a chaque tour (536 870 912 refus en 200 s, bo2_ordonnanceur 0,9 coeur). Une
// coroutine en CS-wait attend un signal sans delai : elle n'interdit plus le repos, le demande elle-meme, et le fil
// heberge qui rend la section reveille l'ordonnanceur. Un signal manque coute au plus la borne de 1 ms.
static uint32_t g_cs_waiting_for[64];            // section qu'attend chaque coroutine (CS-wait), 0 = aucune
static std::atomic<int> g_ordo_cs_attentes{0};   // coroutines du tourniquet en CS-wait (niveau 3)
static dispatch_semaphore_t ordo_sem() {
    static dispatch_semaphore_t s = dispatch_semaphore_create(0);
    return s;
}
static int ordo_repos_niveau() {
    static const int v = [] {
        const char* e = bo2_getenv("BO2_ORDO_REPOS");
        const int defaut = 1;
        const int n = e ? atoi(e) : defaut;
        fprintf(stderr, "[ORDO-REPOS] repos du fil ordonnanceur : niveau %d (BO2_ORDO_REPOS : 0 desarme, "
                        "1 Sleep/Present-sync, 2 + WFSO-event, 3 + CS-wait)\n", n);
        return n;
    }();
    return v;
}
// Dort jusqu'a la plus proche echeance (1 ms au plus) si aucune AUTRE coroutine du tourniquet ne peut avancer.
static void ordo_repos_si_inactif(const GThread* me, uint64_t deadline, const char* why) {
    static uint64_t n_repos = 0, ns_repos = 0;
    uint64_t ech = deadline;
    const int n = (int)sched_nthreads();
    const bool cs_attend = ordo_repos_niveau() >= 3;
    for (int i = 0; i < n; i++) {
        const GThread* t = g_threads[i];
        if (t == me || t->hosted) continue;
        const int st = __atomic_load_n(&t->status, __ATOMIC_RELAXED);
        const bool sondeur_vide = ordo_sonde_on() && i < 32 && g_poll_idle[i] >= 2;   // voir coop_yield_from
        const uint64_t w = st == T_RUN ? (((cs_attend && i < 64 && g_cs_waiting_for[i]) || sondeur_vide) ? ~0ull
                                                                                                   : t->sleep_until_ns)
                         : (st == T_BLOCKED && t->wake_at) ? t->wfso_until_ns : ~0ull;
        if (!w) {   // qui interdit le repos (Z-NATIF-14y) ; fil ordonnanceur seul, donc compteurs simples
            static uint64_t n_refus[32] = {};
            const uint64_t k = i < 32 ? ++n_refus[i] : 0;
            if (k >= 1024 && (k & (k - 1)) == 0)
                fprintf(stderr, "[ORDO-REPOS refus #%llu] tid=%d %s cession=%s ; demandeur tid=%d %s\n",
                        (unsigned long long)k, i, st == T_RUN ? "RUN sans echeance" : "BLK a delai fini",
                        g_yield_last[i] ? g_yield_last[i] : "-", (int)g_cur, why);
            return;
        }
        if (w < ech) ech = w;
    }
    const uint64_t now = bo2_mono_ns();
    if (ech <= now) {
        static uint64_t n_passe = 0;
        const uint64_t k = ++n_passe;
        if (k >= 1024 && (k & (k - 1)) == 0)
            fprintf(stderr, "[ORDO-REPOS echeance passee #%llu] demandeur tid=%d %s\n",
                    (unsigned long long)k, (int)g_cur, why);
        return;
    }
    const uint64_t d = std::min<uint64_t>(ech - now, 1000000ull);
    dispatch_semaphore_wait(ordo_sem(), dispatch_time(DISPATCH_TIME_NOW, (int64_t)d));
    ns_repos += bo2_mono_ns() - now;
    const uint64_t k = ++n_repos;
    if (k >= 1024 && (k & (k - 1)) == 0)   // puissances de deux : #N est le VRAI compte
        fprintf(stderr, "[ORDO-REPOS #%llu] tid=%d %s : %.0f ms de repos au total\n",
                (unsigned long long)k, (int)g_cur, why, (double)ns_repos / 1e6);
}
static uint32_t ordo_attendre_jusqua(const char* why, uint64_t deadline) {
    GThread* me = (g_cur >= 0 && g_cur < (int)sched_nthreads()) ? g_threads[g_cur] : nullptr;
    if (me) me->sleep_until_ns = deadline;
    uint32_t tours = 0;
    do {
        if (me && ordo_repos_niveau() >= 1) ordo_repos_si_inactif(me, deadline, why);
        coop_yield_from(why); ++tours;
    } while (bo2_mono_ns() < deadline);
    if (me) me->sleep_until_ns = 0;
    return tours;
}

extern "C" void win32_coop_yield() { coop_yield_from("audio-throttle"); }
extern "C" void win32_coop_yield_reason(const char* why) { coop_yield_from(why); }

// ── 🔴 P628/E4 — LA RECULADE, EXPOSEE AUX SITES DE win32_runtime.cpp ─────────
//
// Rend `true` si l'appelant est un fil HOTE et qu'on a recule a sa place.
// Un seul client aujourd'hui : `cas_spin_note`, le detecteur de spin sur
// `cmpxchg`. Pourquoi il en a besoin, et pourquoi la reponse n'etait PAS de
// rendre ses compteurs thread_local :
//
// `cas_spin_note` compte les echecs consecutifs sur la MEME adresse, et remet
// sa serie a zero si `g_guest_writes` a bouge entre deux essais — « ce fil a
// travaille entre-temps, ce n'est donc pas un spin ». Avec six fils hotes qui
// ecrivent ~105 M fois par seconde dans ce MEME compteur global, il bouge entre
// absolument tous les essais : **la serie ne peut plus jamais atteindre 8, donc
// le detecteur ne se declenche JAMAIS** et un fil hote spinne indefiniment sur
// un `cmpxchg` a pleine vitesse de cœur. Meme maladie que E7, autre site.
//
// ⚠️ ET LE CORRECTIF « EVIDENT » ETAIT UN PIEGE. Passer `g_guest_writes` et
// `g_wr_hist` en `thread_local` (c'etait le plan E4 d'origine) aurait mis un
// acces TLS sur le chemin des 105 M d'ecritures/s. Sur Darwin, `thread_local`
// ne se compile PAS en local-exec : clang emet un descripteur `tlv` et un APPEL
// a `_tlv_get_addr`. Ce fichier s'en mefie deja explicitement — le proxy
// `SchedCur` existe pour eviter un acces TLS a 4 251 lectures/s, en disant « un
// acces TLS n'est pas garanti gratuit sur macOS ». Le poser a 105 M/s aurait
// ete la meme faute, cinq ordres de grandeur plus cher, et dans le regime PAR
// DEFAUT — celui qu'on ne mesure pas dans cet A/B.
// ⇒ on ne touche pas au chemin chaud. On donne au detecteur, sur un fil hote,
//   une serie qui lui est propre et une vraie reculade — et l'acces TLS n'est
//   paye qu'a l'ECHEC d'un `cmpxchg`, pas a chaque ecriture.
extern "C" int bo2_host_backoff_if_hosted(void) {
    if (__builtin_expect(!g_any_hosted, 1) || !k32_hors_ordonnanceur() || t_host_tid < 0) return 0;
    host_backoff(t_host_tid);
    return 1;
}

// P648/C2 : la coroutine `tid` tourne-t-elle sur son propre fil hote ? (et le fil
// appelant en est-il un ?) Pour audio_preempt, qui ne cede qu'au profit du mixeur.
extern "C" int bo2_tid_hosted(int tid) {
    if (__builtin_expect(!g_any_hosted, 1)) return 0;
    return (tid >= 0 && tid < (int)sched_nthreads() && g_threads[tid]->hosted) ? 1 : 0;
}
extern "C" int bo2_self_hosted(void) {
    return (__builtin_expect(g_any_hosted, 0) && k32_hors_ordonnanceur() && t_host_tid >= 0) ? 1 : 0;
}

// ── BO2_PARK_WILD=1 — RETIRE A THREAD WHOSE STACK IS PROVABLY DESTROYED ──────
//
// tid 5 spins in sub_8fc330 with a NULL queue object and an esp of -0x414: the
// stack pointer was zeroed and one prologue ran on it. The scheduler is
// cooperative, so that one thread freezes the other thirteen — tid 12 (SV_Frame)
// included — and `advancing=NO` is simply "the server frame is never run".
//
// This does NOT repair the corruption; the `leave` with ebp = 0 that causes it is
// still unfound. It answers one question the rest of the work depends on: with
// tid 5 out of the way, does the campaign tick? A thread whose esp is outside the
// bounds thread_trampoline handed it is destroyed by a deductive check, not a
// heuristic — every later reading taken from it is already fiction — so retiring
// it costs nothing that still worked.
//
// The park must happen on the GUEST pthread: coop_yield_from swaps contexts, and
// calling it from the host watchdog would swap the wrong stack. So the watchdog
// only RAISES a flag and the guest retires itself at its next memory read.
int g_park_pending BO2_HOT_LINE = 0;          // hot-path flag: plain int, read-only there
BO2_HOT_PAD(k32);   // ferme __bo2_hot pour cette unite (bo2_hot.h)
static std::atomic<int> g_park_tid{-1};
// ⚠️ A WILD ESP ALONE IS NOT PROOF OF DEATH. The first version parked on the
// first sighting and retired tid 5, 6 AND 8 during boot — the level then never
// loaded. Threads pass through an esp outside their registered stack legitimately
// (a thread running on a stack it was not handed is a different bug from a thread
// that is finished), so the flag has to be a CONJUNCTION:
//
//   esp outside the registered stack  AND  esp identical across N watchdog passes
//
// A thread that is merely visiting an odd stack keeps moving; a destroyed one is
// frozen at the same word forever. Requiring both, and parking at most ONE thread
// per run, is what separates them.
extern "C" void win32_request_park(int tid, uint32_t esp) {
    if (!bo2_getenv("BO2_PARK_WILD")) return;
    static uint32_t prev[64] = {0};
    static int hits[64] = {0};
    static bool spent = false;
    if (spent || tid < 0 || tid >= 64) return;
    // ⚠️ THE REGISTERED BOUNDS ARE NOT TRUSTWORTHY. Measured: [ESP-WILD] fires for
    // tid 1, 10, 11, 12, 13 and 14 at esp values that are plainly fine
    // (0x422d5dd4, 0x4e5daba8, ...), and the first qualified park spent itself on
    // tid 1, which was healthy. So `t->guest_stack` does not describe where these
    // threads actually run, and "outside the registered stack" is a false-positive
    // machine — a caveat that applies to every [ESP-WILD] line ever logged.
    //
    // What is NOT arguable is an esp no stack can have. The guest arena puts every
    // real stack in the low 4 GB well away from both ends, so esp below 64 KB or
    // above 0xF0000000 is destroyed by arithmetic, not by a bounds table. tid 5's
    // 0xfffffd5c (= -676) qualifies; every false positive above does not.
    if (esp >= 0x10000u && esp < 0xf0000000u) return;
    const int need = [] { const char* e = bo2_getenv("BO2_PARK_WILD"); int v = e ? atoi(e) : 1; return v > 1 ? v : 3; }();
    if (prev[tid] == esp) ++hits[tid]; else { hits[tid] = 1; prev[tid] = esp; }
    if (hits[tid] < need) {
        fprintf(stderr, "      [PARK] tid %d wild at esp=0x%08x, unchanged %d/%d pass(es) — "
                "not parked yet\n", tid, esp, hits[tid], need);
        return;
    }
    spent = true;
    g_park_tid.store(tid);
    g_park_pending = 1;
    fprintf(stderr, "      [PARK] tid %d wild AND frozen at esp=0x%08x for %d passes — "
            "asking it to retire at its next memory read\n", tid, esp, hits[tid]);
}
extern "C" void win32_park_check() {
    const int want = g_park_tid.load(std::memory_order_relaxed);
    if (want < 0 || g_cur != want) return;
    g_park_pending = 0;
    g_park_tid.store(-1);
    fprintf(stderr, "\n🎯 [PARK] tid %d retired mid-loop: its esp is outside the stack "
            "thread_trampoline gave it, so it was destroyed rather than blocked. The "
            "other threads can now be scheduled — watch whether advancing goes YES.\n\n",
            (int)g_cur);
    // 🎯 A DESTROYED THREAD MAY STILL HOLD A LOCK, AND RETIRING IT ALONE THEN
    // GUARANTEES THE DEADLOCK IT WAS CAUSING.
    //
    // Measured after the first park: tid 0 sits in the backoff loop at 0x5cb6c0
    // — `try-acquire; Sleep; retry`, with ebp = [0xa3b0f8] = KERNEL32!Sleep — so
    // it yields properly and is NOT hogging. It is waiting for the lock passed at
    // 0x4b4886, which is `*(0xea0560) + 0x10`. And tid 0 is the only thread that
    // signals event 0x10039, which tid 12 (SV_Frame) waits on. So the chain is:
    //
    //     tid 5 dies holding the lock -> tid 0 waits for it forever
    //       -> tid 0 never SetEvents 0x10039 -> SV_Frame never runs -> advancing=NO
    //
    // Report the lock's contents on the way out. Releasing it is a separate,
    // louder decision (BO2_PARK_WILD>=10) because publishing a lock the dead
    // thread may have been half-way through mutating trades one corruption for
    // another — worth measuring, not worth doing silently.
    {
        const uint32_t obj  = win32_mem_read32(0xea0560u);
        const uint32_t lock = obj ? obj + 0x10u : 0u;
        fprintf(stderr, "[PARK] the lock at *(0xea0560)+0x10 = 0x%08x reads "
                "[0x%08x 0x%08x 0x%08x 0x%08x]\n", lock,
                lock ? win32_mem_read32(lock) : 0,
                lock ? win32_mem_read32(lock + 4) : 0,
                lock ? win32_mem_read32(lock + 8) : 0,
                lock ? win32_mem_read32(lock + 12) : 0);
        // Its OWN switch: overloading BO2_PARK_WILD to mean both "passes required"
        // and "also release" cost a run — setting it to 10 silently raised the
        // threshold to ten passes, which 150 s never reached.
        if (lock && bo2_getenv("BO2_PARK_RELEASE")) {
            win32_mem_write32(lock, 0);
            fprintf(stderr, "[PARK] ⚠️ released it (BO2_PARK_WILD>=10) — an EXPERIMENT: "
                    "if tid 0 now runs and advancing goes YES, the lock was the last link\n");
        }
    }
    g_threads[g_cur]->status = T_DONE;
    coop_yield_from("park-wild");
}

// Current coroutine index, for tid-tagging cross-thread traces (ring tracer).
extern "C" int win32_current_tid() { return g_cur; }

// Sommet de la pile HOTE sur laquelle tourne l'appelant : pile de coroutine (malloc de COSTACK) ou pile du fil
// (heberge, principal). Borne les remontees de cadres des sondes : le cadre le plus externe d'une coroutine
// pointe sur ce sommet, et le suivre lisait ce qui est mappe au-dessus (Z-NATIF-18d).
extern "C" uintptr_t bo2_host_stack_top() {
    const uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    const int c = g_cur;
    if (c >= 0 && c < (int)sched_nthreads()) {
        const uintptr_t lo = (uintptr_t)g_threads[c]->hstack;
        if (lo && sp >= lo && sp < lo + COSTACK) return lo + COSTACK;
    }
    return (uintptr_t)pthread_get_stackaddr_np(pthread_self());
}

static void impl_GetCurrentThreadId(State& s) {
    sched_init();
    s.gpr.rax.dword = 0x1000u + (uint32_t)g_cur;   // distinct per guest thread
    win32_stdcall_ret(s, 0);
}

// Guest longjmp shim (replaces the lifted 0x965790 via a post-link entry patch).
// The engine — and all of Lua (lua_pcall/luaD_throw), which the entire LUI menu
// runs on — uses MSVC setjmp/longjmp for error recovery. The lifted longjmp
// derails in the MSVC SEH-unwind walk our environment can't honor, so bypass it:
// read the _JUMP_BUFFER (Ebp,Ebx,Edi,Esi,Esp,Eip at +0,4,8,0xC,0x10,0x14),
// restore the guest registers, and host-longjmp to this thread's dispatch root,
// which re-dispatches at the saved EIP. Called with the lifted-fn ABI so the
// patched `bl _sub_965790` reaches it unchanged. cdecl: longjmp(env, value) —
// args at [esp+4]/[esp+8]; longjmp never returns.
extern "C" void bo2_lua_report_error(const char* why, uint32_t env);
extern uint32_t g_sj_ebp[64];   // win32_runtime.cpp: the live ebp at the last _setjmp3
struct LjRec { uint32_t env, from, esp, jesp, jeip, jebp, jebx, jedi, jesi, sjebp; uint64_t sjn; };
static LjRec g_lj_ring[64][4]; static uint32_t g_lj_n[64];
extern "C" __attribute__((used)) void* bo2_longjmp(State* s, uint32_t /*pc*/, void* /*mem*/) {
    sched_init();
    uint32_t esp = s->gpr.rsp.dword;
    uint32_t env = win32_mem_read32(esp + 4);
    uint32_t val = win32_mem_read32(esp + 8);
    // 🎯 WHO THREW? The campaign spmap call never returns because a longjmp
    // unwinds past its frame (spmap esp=0x0010eccc, longjmp lands at 0x0010f0c0
    // — 1,012 bytes SHALLOWER, so the frame is abandoned and the mission startup
    // that follows the load never runs). The longjmp itself is correct; the
    // anomaly is that something THROWS 127 times during a campaign load and zero
    // times before it. The return address at [esp] names the thrower, and
    // nothing else can: Com_Printf is not interposed, so the engine's own error
    // message goes nowhere we can read.
    {
        static const long lim = [] {
            const char* e = bo2_getenv("BO2_LONGJMP_WHO");
            return e ? (atol(e) > 0 ? atol(e) : 12L) : 0L;
        }();
        // ⚠️ FILTER BY STACK. The first version logged the first 12 longjmps of the
        // run and every one was on a 0x456d.... stack — a secondary guest thread
        // (Lua's pcall path), not the one that matters. The throw that abandons
        // the spmap frame lands on the MAIN stack, around 0x0010xxxx. Without the
        // filter the interesting family is drowned by the noisy one.
        const uint32_t land = win32_mem_read32(env + 0x10) + 4;
        if (lim && (land >> 16) == 0x0010u) {
            static long n = 0;
            // 🎯 AND THE DISPATCH TRAIL AT THE FIRST ONE. The campaign spmap frame is
            // abandoned by this throw, so whatever the crumbs show here is the last
            // thing the map load was doing before control left it for good. One
            // dump only: the trail is 128 entries and repeats after that.
            if (n == 0) {
                extern void win32_dump_crumbs(int);
                fprintf(stderr, "[LONGJMP-WHO] the dispatch trail at the FIRST "
                                "main-stack throw — this is where the map load "
                                "lost control:\n");
                win32_dump_crumbs(20);
            }
            if (++n <= lim)
                fprintf(stderr, "[LONGJMP-WHO #%ld] thrown from ret=0x%08x env=0x%08x "
                                "val=%d (restores esp=0x%08x eip=0x%08x)\n",
                        n, win32_mem_read32(esp), env, (int)val,
                        win32_mem_read32(env + 0x10) + 4, win32_mem_read32(env + 0x14));
        }
    }
    s->gpr.rbp.dword = win32_mem_read32(env + 0x00);
    s->gpr.rbx.dword = win32_mem_read32(env + 0x04);
    s->gpr.rdi.dword = win32_mem_read32(env + 0x08);
    s->gpr.rsi.dword = win32_mem_read32(env + 0x0C);
    // 🎯🎯🎯 THE +4 IS NOT OPTIONAL — IT IS THE `ret` MSVC's longjmp PERFORMS.
    //
    // _setjmp3 (0x964060), disassembled:
    //     0x96406f  mov [edx+0x10], esp   <- esp AT ENTRY: points AT the return address
    //     0x964072  mov eax, [esp]
    //     0x964075  mov [edx+0x14], eax   <- Eip = that same return address
    //
    // So the saved Esp INCLUDES the return-address slot. On a normal setjmp
    // return the `ret` pops it, leaving esp = Esp + 4; MSVC's longjmp reproduces
    // that state by writing Eip into the slot and executing `ret`. Restoring
    // esp = Esp and dispatching at Eip leaves the guest 4 bytes LOW — every
    // longjmp, compounding, because the site re-runs setjmp at the new esp.
    //
    // Measured before this fix (BO2_UNWIND_LOG=1, same jmpbuf 0x02f43e40, Ebp
    // constant): Esp 0x0010e8c4 -> 8c0 -> 8bc -> 8b8, exactly -4 each time. After
    // enough drift the GSC VM's epilogue (`pop edi; pop esi; pop ebx`) pops the
    // wrong words, esi comes back as a variable index instead of the VM instance,
    // and the campaign dies on a FALSE "exceeded maximum number of child client
    // script variables" with 57,209 of 65,536 records still free.
    //
    // BO2_LONGJMP_NOPOP=1 restores the old (broken) behaviour for A/B.
    // 🎯 BO2_LJ_GUARD=1 — REFUSE A LONGJMP THROUGH A JMPBUF NO SETJMP EVER FILLED.
    //
    // A jmpbuf whose Esp AND Eip are both zero was never written by _setjmp3.
    // Jumping through it cannot recover anything: it restores esp = 0 + 4 and
    // dispatches at pc 0, which is precisely how tid 5 dies —
    //   "Default sound assets not found" -> sub_444650 -> 0x444894 call longjmp
    //   -> esp = 4 -> the thread runs on a zeroed stack, wedges in a null-object
    //   dequeue, and starves the other thirteen coroutines.
    // So performing it is never right, and the only question is what to do
    // instead. The guest cannot continue either — 0x444899 is `int3`, the code
    // after the call is unreachable by construction — so the honest action is to
    // retire this ONE thread and leave the other thirteen running, rather than
    // let a dead thread take the whole scheduler with it.
    //
    // This does NOT fix the missing asset; it stops one engine-level error from
    // costing the entire process.
    {
        const uint32_t j_esp = win32_mem_read32(env + 0x10);
        const uint32_t j_eip = win32_mem_read32(env + 0x14);
        // ring of the last 4 throws per thread, dumped at [unwind-badret]: the
        // fatal throw is one in thousands and every print cap missed it twice.
        if (g_cur >= 0 && g_cur < 64) {
            LjRec& r = g_lj_ring[g_cur][g_lj_n[g_cur]++ & 3u];
            r.env = env; r.from = win32_mem_read32(esp); r.esp = esp; r.jesp = j_esp; r.jeip = j_eip; r.jebp = win32_mem_read32(env + 0x00);
            r.jebx = win32_mem_read32(env + 4u); r.jedi = win32_mem_read32(env + 8u); r.jesi = win32_mem_read32(env + 12u);
            { extern uint64_t g_sj_n[64]; r.sjebp = g_sj_ebp[g_cur]; r.sjn = g_sj_n[g_cur]; }
        }
        // 🎯 EVERY thread's longjmp, not only main's. [LONGJMP-WHO] filters on
        // main's stack (Esp>>16 == 0x0010), so the Server's throw -- the one that
        // ends every angola run via [unwind-badret] -> [thread] DONE -- was never
        // logged. A stale jmpbuf (setjmp frame already returned) shows here as
        // j_esp ABOVE the current esp, or an Eip that is not a setjmp site.
        {   static int nlj[64] = {0};   // PER THREAD: tid 5 (the 2nd Lua state) throws thousands of times and a global cap never reached the Server
            if (g_cur >= 0 && g_cur < 64 && nlj[g_cur]++ < 24)
            {   extern uint32_t g_sj_env[64], g_sj_ret[64], g_sj_esp[64]; extern uint64_t g_sj_n[64];
                int poser = -1; for (int t = 0; t < 64; t++) if (g_sj_env[t] == env && g_sj_n[t]) { poser = t; break; }
                const int me = (g_cur >= 0 && g_cur < 64) ? g_cur : 0;
                fprintf(stderr, "[LONGJMP-ALL] tid=%d env=0x%08x from=0x%08x val=%d | now esp=0x%08x | jb: esp=0x%08x eip=0x%08x ebp=0x%08x | "
                                "this tid's last setjmp: env=0x%08x at=0x%08x esp=0x%08x #%llu | env last posed by tid=%d%s\n",
                        (int)g_cur, env, win32_mem_read32(esp), (int)val, esp, j_esp, j_eip, win32_mem_read32(env + 0x00),
                        g_sj_env[me], g_sj_ret[me], g_sj_esp[me], (unsigned long long)g_sj_n[me], poser,
                        (esp > j_esp) ? "   <- current esp ABOVE jb.esp: that frame has already RETURNED (stale jmpbuf)" : "");
                // Z-NATIF-09o : Zombies n'a pas la voix du moteur. Les cadres que ce longjmp abandonne tiennent encore
                // les chaines de leurs appelants (le texte Lua derriere « UI Error <condense> ») : les nommer.
                for (uint32_t a = esp; a < j_esp && a < esp + 0x400u; a += 4) {
                    const uint32_t p = win32_mem_read32(a);
                    char t[240]; int k = 0;
                    for (; p >= 0x10000u && k < 239; k++) {
                        const uint8_t c = win32_mem_read8(p + (uint32_t)k);
                        if (c < 0x20 || c > 0x7e) break;
                        t[k] = (char)c;
                    }
                    t[k] = 0;
                    if (k >= 12) fprintf(stderr, "[LONGJMP-PILE] tid=%d [esp+%#x]=%08x \"%s\"\n", (int)g_cur, a - esp, p, t);
                }
#if !defined(BO2_TARGET_SPS)   // match_rules.cpp, which defines it, is not linked into the campaign (issue #5)
                {   // Erreur de VM : la position du script (variable du cadre de VM_Execute) se lit sur la pile.
                    int vus = 0;
                    for (uint32_t k = 1; k <= 3 && vus < 3; k++) vus += bo2_regles_situe(win32_mem_read32(env + 4 * k));
                    for (uint32_t a = esp; a < j_esp + 0x200u && vus < 3; a += 4) vus += bo2_regles_situe(win32_mem_read32(a));
                }
#endif
            }
        }
        if (j_esp == 0 && j_eip == 0 && bo2_getenv("BO2_LJ_GUARD")) {
            fprintf(stderr, "\n🎯 [LJ-GUARD] tid %d: longjmp(env=0x%08x) REFUSED — the jmpbuf is "
                    "all zeroes, so no setjmp ever registered it. Jumping would set esp=4 and "
                    "pc=0. Retiring this thread instead; the other coroutines keep running.\n\n",
                    (int)g_cur, env);
            // 🩹 BO2_LJ_RELEASE=1 — CONTAINMENT, NOT A FIX. Retiring the thread
            // stops it killing the process, but it keeps whatever that thread was
            // holding, and on 2026-09-05 that was measured to be fatal anyway:
            //
            //   tid 5 raises Com_Error("Invalid sound handle") inside
            //   updatesound_worker -> sub_886a20 -> sub_60dac0, is retired HERE,
            //   and still holds the sound lock [0x300daa8]=1 and that job's
            //   "running" count [0xea05a4]=1. Main then spins for ever in
            //   Sound_Lock (sub_4b4860) waiting for a job whose only runner is
            //   gone: no frame 3847, [RUNAWAY] exactly 3,000,000 imports later.
            //
            // So LJ_GUARD alone turned "one dead thread kills the process" into
            // "one dead thread deadlocks it". This releases the two things that
            // thread is known to hold. It does NOT repair the invalid sound
            // handle — the engine still failed — and it is deliberately opt-in so
            // a run without it stays comparable.
            //
            // ⚠️ ON A REPLAY THIS WILL DIVERGE, BY CONSTRUCTION: if it works, the
            // guest stops spinning and takes a different path, so the journal's
            // remaining records no longer match. Judge it by "did the run pass
            // frame 3847", not by JRNL-DIVERGE.
            if (bo2_getenv("BO2_LJ_RELEASE")) {
                const uint32_t lock = win32_mem_read32(0x0300daa8u);
                const uint32_t run  = win32_mem_read32(0x00ea05a4u);
                if (lock) win32_mem_write32(0x0300daa8u, 0u);
                if (run)  win32_mem_write32(0x00ea05a4u, run - 1u);
                fprintf(stderr, "🩹 [LJ-RELEASE] tid %d retired while holding the sound lock "
                        "[0x300daa8]=%u and updatesound_worker running=[0xea05a4]=%u — both "
                        "released so the main thread's Sound_Lock can proceed. CONTAINMENT "
                        "ONLY: the \"Invalid sound handle\" that killed this thread is "
                        "unfixed.\n", (int)g_cur, lock, run);
            }
            // 🎯 BO2_LJ_RESTART=1 — RESTART THE WORKER INSTEAD OF RETIRING IT.
            //
            // Retiring is what makes this fatal rather than merely noisy. Measured
            // 2026-09-05: tid 5 raises Com_Error("Invalid sound handle") inside
            // updatesound_worker, is retired here, and it ran 3308 of that job's
            // 3466 calls — so removing it removes the thread that actually
            // services the job, and Main waits for the next submission for ever.
            // Releasing the counters alone is not enough either: it only moves the
            // stall from (pending 0, running 1) to (pending 1, running 0), and the
            // run still stops at frame 3846.
            //
            // The landing machinery already exists and is proven — thread_trampoline
            // does `setjmp(t->dispatch_jb)` for EVERY guest thread and bo2_longjmp
            // returns there ~1364 times a run for Lua pcall unwinds. What is missing
            // is only a valid GUEST resume point, because Com_Error's jmpbuf was
            // never filled on a worker. So supply one: the thread's own entry, with
            // the exact frame thread_trampoline builds (param at [esp+4], the
            // 0xDEAD0000 sentinel at [esp], ebp seeded to esp).
            //
            // ⚠️ THIS IS A RECOVERY, NOT A REPAIR. The engine still hit a real
            // error, and a restarted worker may be re-entering with guest state the
            // abandoned job left half-updated. It is opt-in for exactly that reason,
            // it says so in its own log line, and it is only worth using together
            // with BO2_LJ_RELEASE, which puts the abandoned job's accounting back.
            if (bo2_getenv("BO2_LJ_RESTART") && g_threads[g_cur]->dispatch_armed
                && g_threads[g_cur]->guest_stack) {
                GThread* rt = g_threads[g_cur];
                const uint32_t esp0 = rt->guest_stack + rt->guest_stack_size - 0x20;
                win32_mem_write32(esp0 + 4, rt->param);
                win32_mem_write32(esp0 + 0, 0xDEAD0000u);
                s->gpr.rsp.dword = esp0;
                s->gpr.rbp.dword = esp0;
                s->gpr.rax.dword = 0;
                rt->resume_pc  = rt->proc;
                rt->unwind_env = env;
                fprintf(stderr, "🎯 [LJ-RESTART] tid %d restarted at its entry proc=0x%08x "
                        "param=0x%08x esp=0x%08x instead of being retired. RECOVERY, NOT A "
                        "REPAIR: the engine error that got here is unfixed.\n",
                        (int)g_cur, rt->proc, rt->param, esp0);
                _longjmp(rt->dispatch_jb, 1);         // unwinds host frames; no return
            }
            g_threads[g_cur]->status = T_DONE;
            coop_yield_from("longjmp-dead-jmpbuf");
            return nullptr;
        }
    }
    {
        static const bool nopop = bo2_getenv("BO2_LONGJMP_NOPOP") != nullptr;
        s->gpr.rsp.dword = win32_mem_read32(env + 0x10) + (nopop ? 0u : 4u);
    }
    s->gpr.rax.dword = val ? val : 1;               // longjmp(env,0) returns 1
    uint32_t eip = win32_mem_read32(env + 0x14);
    // 🎯 A JMPBUF THAT RESTORES AN IMPOSSIBLE ESP — ALWAYS ON, THE EVENT IS FATAL.
    //
    // This is how tid 5 dies. The last healthy guest return is to 0x00444890, and
    // the very next instructions are
    //     0x444893  push eax          ; eax = sub_670220(2,-1), a jmpbuf
    //     0x444894  call 0x965790     ; longjmp  <- us
    //     0x444899  int3              ; unreachable, longjmp never returns
    // after which esp is 0xfffffbec — zero minus one frame. So the stack was not
    // overflowed or walked down: it was RESTORED from a jmpbuf that no setjmp had
    // filled, and the thread then ran on it until it wedged in a null-object
    // dequeue and starved the other thirteen coroutines.
    //
    // [LJ-BUF] could never see this: it is gated on eip == 0x87f852, the GSC VM's
    // resume, and this site is not that one. Gate on the RESTORED VALUE instead —
    // an esp below 64 KB or above 0xfff00000 is impossible for any live guest
    // stack, so this cannot false-positive the way a bounds table does.
    {
        const uint32_t new_esp = s->gpr.rsp.dword;
        if (new_esp < 0x00010000u || new_esp >= 0xfff00000u) {
            static uint32_t n = 0;
            if (n++ < 8) {
                // g_threads directly: win32_thread_stack_range is defined further
                // down this file and is not visible here.
                uint32_t lo = 0, hi = 0;
                if (g_cur >= 0 && g_cur < (int)sched_nthreads()) {
                    lo = g_threads[g_cur]->guest_stack;
                    hi = lo ? lo + g_threads[g_cur]->guest_stack_size : 0;
                }
                fprintf(stderr, "\n🎯 [LJ-DEAD] tid %d: longjmp(env=0x%08x) restored an "
                        "IMPOSSIBLE esp=0x%08x — this thread's stack is now gone.\n"
                        "   jmpbuf: Ebp=%08x Ebx=%08x Edi=%08x Esi=%08x Esp=%08x Eip=%08x\n"
                        "   its own stack is [0x%08x,0x%08x); caller esp was 0x%08x\n"
                        "   => the jmpbuf was never filled by a setjmp (or was freed).\n\n",
                        (int)g_cur, env, new_esp,
                        win32_mem_read32(env + 0x00), win32_mem_read32(env + 0x04),
                        win32_mem_read32(env + 0x08), win32_mem_read32(env + 0x0C),
                        win32_mem_read32(env + 0x10), eip, lo, hi, esp);
            }
        }
    }
    // 🎯 WHERE DOES THE 4-BYTE-PER-LONGJMP DRIFT COME FROM?
    //
    // Measured: six consecutive GSC error longjmps on tid 12 resume with esp
    // 4 bytes lower each time (0x4e5d99b4 -> 0x4e5d99a0), and tid 0 shows the
    // same signature. That esp IS the jmpbuf's saved Esp, so SETJMP is being
    // re-executed at a progressively lower esp — the leak is upstream of
    // longjmp, not inside it.
    //
    // After enough drift the VM's epilogue (`pop edi; pop esi; pop ebx`) pops
    // the wrong words, esi comes back as a variable index instead of the VM
    // instance, and the campaign dies on a false "exceeded maximum number of
    // child client script variables".
    //
    // This prints the whole jmpbuf per longjmp so the drift can be attributed:
    // same `env` with a falling Esp means one setjmp site re-running lower each
    // time; different `env` means distinct sites and the drift is in the guest's
    // own nesting. Always on, capped, tid-tagged — the event is rare (6 per run)
    // and it is the one this hunt turns on.
    {
        // ⚠️ GATE ON THE RESUME SITE, NOT ON A COUNT. The first version capped at
        // 24 and tid 5 — which longjmps 3,747 times through its OWN setjmp at
        // 0x6404e0 — consumed every slot long before tid 12 longjmps at all.
        // Third capped-probe miss of this session. tid 5's jmpbuf Esp is constant
        // (0x456d0138, delta 0 every time), so it is not the drifting one anyway;
        // 0x87f852 is the GSC VM's setjmp resume and the only site that matters.
        static int n = 0;
        if (eip == 0x87f852u && n < 40) {
            ++n;
            static uint32_t prev_esp[64] = {0};
            const int t_ = g_cur & 63;
            const uint32_t jesp = win32_mem_read32(env + 0x10);
            const int32_t d = prev_esp[t_] ? (int32_t)(jesp - prev_esp[t_]) : 0;
            fprintf(stderr, "[LJ-BUF #%d] tid=%d env=0x%08x  Ebp=%08x Ebx=%08x Edi=%08x "
                    "Esi=%08x Esp=%08x Eip=%08x  | caller esp=%08x delta_from_prev=%+d\n",
                    n, (int)g_cur, env, win32_mem_read32(env + 0x00),
                    win32_mem_read32(env + 0x04), win32_mem_read32(env + 0x08),
                    win32_mem_read32(env + 0x0C), jesp, eip, esp, d);
            prev_esp[t_] = jesp;
        }
    }
    if (eip == 0x63feffu) {   // Com_Frame's setjmp resume = a Com_Error longjmp
        static int s_ce = 0;
        if (s_ce++ < 10)
            fprintf(stderr, "[COM-ERROR] longjmp->0x63feff val=%u caller-ret=0x%08x args:[%08x %08x %08x] tid=%d\n",
                    val, win32_mem_read32(esp), win32_mem_read32(esp + 0xc),
                    win32_mem_read32(esp + 0x10), win32_mem_read32(esp + 0x14), (int)g_cur);
    }
    GThread* t = g_threads[g_cur];
    if (!t->dispatch_armed) {                        // no root to unwind to (e.g. main,
        fprintf(stderr, "[longjmp] tid=%d UNARMED env=0x%08x eip=0x%08x — cannot unwind\n",
                (int)g_cur, env, eip);                    // not yet wrapped) — bail loudly
        return nullptr;
    }
    t->resume_pc = eip;
    t->unwind_env = env;
    _longjmp(t->dispatch_jb, 1);                      // unwinds host frames; no return
}

// Keep the running thread's State pointer fresh (called from the import
// dispatcher) so a deadlock dump can inspect EVERY thread, incl. main.
extern "C" void bo2_ww_arm(uint32_t va);          // win32_runtime.cpp
extern "C" void win32_note_state(State* st) {
    sched_init();
    g_threads[g_cur]->cur_state = st;
    // 🎯 ESP OUTSIDE ITS OWN STACK — always a bug, and it is the one that stops
    // the campaign. Measured (docs/handoff/SP-01): tid 2 was found running with
    // esp=0x4138ebac while the stack it was given is 0x41958a50..0x41998a50 —
    // 6.1 MB below its own base, right on top of the lock-free job-node pool, so
    // its call frames WERE the queue corruption. Nothing detected that, because
    // a guest stack overflow here is silent: there is no guard page, the frames
    // just land in whatever guest_alloc handed out below.
    // One compare per import dispatch, and it names the thread, the offending
    // ESP and the guest return address that got there.
    GThread* t = g_threads[g_cur];
    if (t->guest_stack && t->guest_stack_size) {
        const uint32_t sp = st->gpr.rsp.dword;
        if (sp < t->guest_stack || sp >= t->guest_stack + t->guest_stack_size) {
            static int said[64] = {0};
            const int k = (g_cur >= 0 && g_cur < 64) ? g_cur : 63;
            if (said[k]++ < 3)
                fprintf(stderr, "[STACK-ESCAPE] tid %d esp=0x%08x is OUTSIDE its stack "
                                "0x%08x..0x%08x (by %+d bytes) ret=0x%08x proc=0x%08x\n",
                        (int)g_cur, sp, t->guest_stack, t->guest_stack + t->guest_stack_size,
                        (int)(sp - t->guest_stack), win32_mem_read32(sp), t->proc);
        }
        // 🎯 AND DUMP THE CHAIN WHILE THERE IS STILL A CHAIN TO DUMP.
        //
        // [STACK-ESCAPE] alone cannot name the runaway: it fires from the import
        // dispatcher, so it reports wherever an import happened to be called
        // once ESP was ALREADY out of range. Measured, that was binkw32's
        // BinkOpen/BinkSetMemory — a clue about which subsystem was deep, and
        // useless as a culprit. (That misreading is recorded in SP-01; it would
        // have sent the next session at the video code.)
        //
        // Fire at 3/4 consumed instead, while the frames are all still present,
        // and print the return chain. A runaway shows itself as a short cycle
        // repeating — which is exactly how the 0x80d/0x6cc recursion was
        // identified, from a dump that only happened to exist because the
        // watchdog crashed.
        if (sp >= t->guest_stack && sp < t->guest_stack + t->guest_stack_size / 4) {
            static int deep[64] = {0};
            const int k = (g_cur >= 0 && g_cur < 64) ? g_cur : 63;
            if (deep[k]++ == 0) {
                extern int g_rd_depth[64], g_rd_depth_max[64];
                fprintf(stderr, "[STACK-DEEP] tid %d has used %u KiB of its %u KiB stack "
                                "(esp=0x%08x, base=0x%08x) proc=0x%08x "
                                "0x80d4d0 depth now=%d max=%d — chain:\n",
                        (int)g_cur, (t->guest_stack + t->guest_stack_size - sp) >> 10,
                        t->guest_stack_size >> 10, sp, t->guest_stack, t->proc,
                        (k < 64 ? g_rd_depth[k] : -1), (k < 64 ? g_rd_depth_max[k] : -1));
                int shown = 0;
                for (uint32_t ad = sp; ad < sp + 0x400 && shown < 40; ad += 4) {
                    const uint32_t rv = win32_mem_read32(ad);
                    if (rv >= 0x401000u && rv < 0xa31000u) {
                        fprintf(stderr, "  [deep 0x%08x] ret=0x%08x\n", ad, rv);
                        shown++;
                    }
                }
                // IS IT A CHAIN OR A REPEATING RECORD? The addresses above came
                // out at a constant 28-byte pitch with identical payloads, which
                // is not what call frames look like — but "looks like" has now
                // been wrong twice in this investigation, so print three
                // consecutive 28-byte units raw and let the bytes say it.
                for (int u = 0; u < 3; u++) {
                    const uint32_t base = sp + 0x70u + (uint32_t)u * 28u;
                    fprintf(stderr, "  [unit %d @0x%08x]", u, base);
                    for (int w = 0; w < 7; w++)
                        fprintf(stderr, " %08x", win32_mem_read32(base + 4u * (uint32_t)w));
                    fprintf(stderr, "\n");
                }
                // AND CATCH THE WRITER LIVE. The fill is still running — a
                // quarter of the stack is left — so arm the write-watch BELOW
                // the current ESP, where it has not reached yet. ww_hit prints
                // the host backtrace, the guest registers and the guest return
                // chain, which is exactly the triple that identified the writer
                // into the job queue. Arming after the fact catches nothing,
                // which is why this is here and not in the post-mortem dump.
                const uint32_t ahead = sp - 0x2000u;
                if (ahead > t->guest_stack) {
                    fprintf(stderr, "  [deep] arming write-watch at 0x%08x "
                                    "(0x2000 below esp, ahead of the fill)\n", ahead);
                    bo2_ww_arm(ahead);
                }
            }
        }
    }
}

// Deadlock dump: every guest thread's regs + a return-address scan of its
// stack, plus the render-handshake globals — called by the RUNAWAY guard so
// a livelock names all parties, not just the thread that tripped the cap.
// Every event's signalled state. The render handshake is entirely event-driven
// and all of it happens in lifted code, so this is the only way to see which
// side of a submit/complete pair actually fired.
// A RING OF THE LAST SYNC OPERATIONS, dumped with the event table.
//
// sync_log caps at 6 per (callsite, handle), which is right for tracing a
// startup sequence and useless for a deadlock: what identifies a lost wakeup is
// the LAST few operations on the handle nobody signalled, and those are exactly
// the ones the cap has already thrown away by then.
//
// The campaign zone now loads 270 MB and stops with every thread blocked on a
// handle — tid 3 (the loader, inside DB_LoadXFile) on 0x10012, main on 0x1003a,
// both pool workers on 0x10028 — nothing spinning, nothing corrupt. So the
// question is purely "who was supposed to signal which handle, and did they",
// and this ring answers it in the [RUNAWAY] dump without any extra run.
struct SyncOp { const char* op; uint32_t h, ret; int tid; };
static SyncOp g_sync_ring[64];
static unsigned g_sync_ring_n;
static void sync_note(const char* op, uint32_t h, uint32_t ret, int tid) {
    SyncOp& e = g_sync_ring[g_sync_ring_n++ % 64];
    e.op = op; e.h = h; e.ret = ret; e.tid = tid;
}

extern "C" void win32_dump_events() {
    sched_init();
    fprintf(stderr, "[EVENTS]");
    // P628/E3 : un PARCOURS complet ne survit pas a un rehachage concurrent —
    // contrairement a une reference gardee, un iterateur devient invalide. Les
    // deux rapporteurs prennent donc le verrou PARTAGE pendant toute leur marche.
    std::shared_lock<std::shared_mutex> lk(g_ev_mx);
    for (auto& kv : g_events)
        fprintf(stderr, " 0x%x=%c%s", kv.first, kv.second.signaled ? 'S' : '-',
                kv.second.manual ? "m" : "a");
    fprintf(stderr, "\n");
    // ⚠️ THIS BLOCK WENT MISSING ONCE. It was inserted with a .replace() whose
    // anchor did not match and whose result was not asserted, so the build was
    // green, the run cost 25 minutes, and the ring simply was not there. Same
    // class as the `grep -c 'DQ-STUCK head'` that could only ever return zero.
    const unsigned n = g_sync_ring_n < 64 ? g_sync_ring_n : 64;
    fprintf(stderr, "[SYNC-RING] last %u SetEvent/ResetEvent/WFSO (oldest first):\n", n);
    for (unsigned i = 0; i < n; i++) {
        const SyncOp& e = g_sync_ring[(g_sync_ring_n - n + i) % 64];
        fprintf(stderr, "  %-10s h=0x%-6x tid=%-2d callsite=0x%08x\n",
                e.op ? e.op : "?", e.h, e.tid, e.ret);
    }
}

// The guest stack range spawn_thread handed a coroutine — 0x40000 bytes from the
// guest arena (thread_trampoline: `win32_guest_alloc(0x40000,16)`). Exposed so a
// corruption report can say whether the ESP that did the writing is even on the
// stack this thread was given: a guest running on some OTHER region is a
// different bug from a guest overflowing its own.
extern "C" void win32_thread_stack_range(int tid, uint32_t* lo, uint32_t* hi) {
    if (lo) *lo = 0;
    if (hi) *hi = 0;
    if (tid < 0 || tid >= (int)sched_nthreads()) return;
    if (lo) *lo = g_threads[tid]->guest_stack;
    if (hi) *hi = g_threads[tid]->guest_stack
                ? g_threads[tid]->guest_stack + g_threads[tid]->guest_stack_size : 0;
}

extern "C" void win32_dump_all_threads() {
    sched_init();
    fprintf(stderr, "[DEADLOCK-DUMP] owner[0x2925588]=0x%08x frameDone[0x338f85c]=%u "
            "depth[0x338f860]=%u rendStarted[0x338f850]=%u backendFlag[0x338f858]=%u\n",
            win32_mem_read32(0x2925588), win32_mem_read32(0x338f85c),
            win32_mem_read32(0x338f860), win32_mem_read32(0x338f850) & 0xffu,
            win32_mem_read32(0x338f858) & 0xffu);
    for (size_t i = 0; i < sched_nthreads(); i++) {
        GThread* t = g_threads[i];
        State* st = t->cur_state;
        if (!st) { fprintf(stderr, "  [tid %zu] status=%d (no state yet)\n", i, t->status); continue; }
        uint32_t esp = st->gpr.rsp.dword;
        uint32_t blk = win32_mem_read32(t->tls_va + 0);
        fprintf(stderr, "  [tid %zu] status=%d wait_h=0x%x esp=0x%08x ebp=0x%08x "
                "tlsblk=0x%08x cachedTid=0x%08x rets:[",
                i, t->status, t->wait_h, esp, st->gpr.rbp.dword,
                blk, blk ? win32_mem_read32(blk + 0x60) : 0);
        int shown = 0;
        for (uint32_t ad = esp; ad < esp + 0x800 && shown < 14; ad += 4) {
            uint32_t rv = win32_mem_read32(ad);
            if (rv >= 0x401000u && rv < 0xa31000u) { fprintf(stderr, " %08x", rv); shown++; }
        }
        fprintf(stderr, " ]\n");
        // 🎯 AND THE GENERAL REGISTERS, BECAUSE A COUNTED LOOP IS DIAGNOSED BY ITS
        // COUNTER. sub_4a98c0 is `for (esi = 0; esi < ebp; esi++) sub_7fa710(...)`
        // — bounded, so it can only look like an infinite loop if `ebp` (its arg0,
        // a channel count) is garbage. `sample` says a guest thread sits in it for
        // 730 of 730 samples; esi and ebp say whether it is grinding through two
        // billion iterations or genuinely stuck. Neither the return-address scrape
        // above nor any import-clocked probe can answer that.
        // Printed for EVERY thread and on every dump, so two dumps give the delta
        // — one snapshot of a counter says nothing about whether it is moving.
        fprintf(stderr, "      eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n",
                st->gpr.rax.dword, st->gpr.rcx.dword, st->gpr.rdx.dword,
                st->gpr.rbx.dword, st->gpr.rsi.dword, st->gpr.rdi.dword);
        // 🎯 IS ESP EVEN ON THIS THREAD'S OWN STACK? Measured on angola's third
        // freeze: tid 10 (sound) reported esp=0xfffffffc and then esp=0x0000004c,
        // carrying ecx=0x80000000 — the value `fistp` writes for a NaN or
        // out-of-range float. A thread whose stack pointer is -4 is not waiting
        // for anything; it has been destroyed, and every later reading taken from
        // it (its return chain, its "wait") is fiction.
        // The bounds are the ones thread_trampoline handed it, so this is a
        // deductive check, not a heuristic — and a deductive check beats a vote
        // (GUIDE §9, the zero-page rule).
        if (t->guest_stack && t->guest_stack_size) {
            const uint32_t lo = t->guest_stack, hi = t->guest_stack + t->guest_stack_size;
            if (esp < lo || esp >= hi)
                fprintf(stderr, "      🎯 [ESP-WILD] tid %zu: esp=0x%08x is OUTSIDE its own "
                                "stack [0x%08x,0x%08x) — this thread is destroyed, not blocked\n",
                        i, esp, lo, hi);
                win32_request_park((int)i, esp);   // BO2_PARK_WILD only; the guest retires itself
        }
        // 🎯 RAW STACK WORDS FOR A THREAD THAT IS RUNNING, NOT BLOCKED.
        //
        // The `rets:[...]` scan above keeps only words that look like code
        // addresses, which is exactly wrong when the question is about
        // ARGUMENTS. sub_87e750 is a search loop whose only exit is finding its
        // second argument in a list; the two arguments and the entry count are
        // ordinary integers and the filter drops every one of them.
        //
        // Only for status==0 (runnable) threads with a sane esp, and only 24
        // words — the blocked threads are all sitting in a wait and their stacks
        // say nothing new every time.
        // ⚠️ tid 0 HAS NO guest_stack. The main thread's stack is the guest's
        // own initial one, not something thread_trampoline handed out, so
        // `t->guest_stack` is 0 for it — and the first version of this dump
        // required a non-zero guest_stack, which excluded exactly the thread the
        // measurement was for. ([ESP-WILD] has the same hole and that is why it
        // has never once reported on tid 0.) Accept a plausible esp instead when
        // there are no recorded bounds.
        const bool sane_esp = t->guest_stack
            ? (esp >= t->guest_stack && esp < t->guest_stack + t->guest_stack_size)
            : (esp >= 0x00010000u && esp < 0xfff00000u);
        if (t->status == 0 && sane_esp) {
            fprintf(stderr, "      [stk]");
            for (uint32_t w = 0; w < 24u; w++) {
                if ((w % 8u) == 0 && w) fprintf(stderr, "\n      [stk]");
                fprintf(stderr, " %08x", win32_mem_read32(esp + w * 4u));
            }
            fprintf(stderr, "\n");
        }
    }
}

// The State of the coroutine currently running (for the watchdog to inspect a
// worker mid-spin). g_cur is always the running thread.
extern "C" State* win32_current_guest_state() {
    if (g_cur >= 0 && g_cur < (int)sched_nthreads()) return g_threads[g_cur]->cur_state;
    return nullptr;
}

// The State of a NAMED thread, for a host-side probe that must look at a thread
// which is not the one running. `win32_current_guest_state` answers "who is
// running", which is the wrong question when the thing being chased is a guest
// thread spinning inside one lifted block: it never yields, so it never stops
// being current, and no import-clocked probe ever gets to look at it.
// Returns null for an unknown index or a thread that has not started.
extern "C" State* win32_thread_state(int idx) {
    if (idx >= 0 && idx < (int)sched_nthreads()) return g_threads[idx]->cur_state;
    return nullptr;
}

// A guest coroutine that ends carries away every signal it was still going to
// emit. Name them: an event this thread — and ONLY this thread — has ever
// signalled, left unsignalled at its death, is a promise nobody can keep, and
// any waiter on it waits forever.
//
// Measured 2026-08-01: the post-video black screen is exactly this. The loader
// coroutine (tid 3, proc 0x80dee0) dies in the unwind trampoline for want of a
// lifted block at 0x8fb713; handle 0x10013 was signalled twice in the whole run
// and both times by tid 3; main polls it at 0x6518bf with a 1 s timeout for the
// rest of the process's life and the title screen never comes up.
//
// BO2_ORPHAN_SIGNAL=1 additionally FIRES those events. That is a diagnostic, not
// a fix — it asserts "the dead thread's job had finished" without checking, and
// the honest repair is to lift the missing block so the thread never dies. It
// exists to answer one question: is this event the ONLY thing between here and
// the title screen, or merely the first of many?
extern "C" void win32_report_orphaned_events(int tid) {
    if (tid < 0 || tid >= 32) return;
    static const bool rescue = bo2_getenv("BO2_ORPHAN_SIGNAL") != nullptr;
    const uint32_t mine = 1u << tid;
    std::shared_lock<std::shared_mutex> lk(g_ev_mx);   // P628/E3, cf. win32_dump_events
    for (auto& kv : g_events) {
        GEvent& e = kv.second;
        if (e.signalers != mine || e.signaled) continue;   // shared or already set
        fprintf(stderr, "[ORPHAN-EVENT] h=0x%x unsignalled and tid=%d was its ONLY "
                "signaller — %s\n", kv.first, tid, rescue ? "FIRING it (diagnostic)" : "left alone");
        if (!rescue) continue;
        e.signaled = true;
        for (size_t i = 0, n = sched_nthreads(); i < n; i++) {   // P651/C6 : jamais la fin du vecteur
            GThread* w = g_threads[i];
            if (w->status == T_BLOCKED && w->wait_h == kv.first) { w->status = T_RUN; w->wake_at = 0; }
        }
    }
}

// --- BO2_UNWIND_SKIP: enumerate the WHOLE remaining unwind chain in ONE run.
//
// After a guest longjmp the shim climbs the guest return chain, and every hop has
// to be a REGISTERED lifted entry point — not merely an address inside some lifted
// function. A return site that is only ever reached by falling out of its own
// function has no entry symbol, so the climb stops there and the thread dies. Each
// such hop has cost one full boot to discover: the climb has moved
// 0x883679 -> 0x469b8a -> 0x630d7d -> 0x56fc89 -> 0x686ca7, five runs for five
// addresses, because only the address that happened to be next was ever reachable.
//
// With BO2_UNWIND_SKIP=1 the climb, instead of dying, hunts up the guest stack for
// the next return address that IS lifted and resumes there — abandoning the frames
// in between. That is only defensible BECAUSE this path is already the
// abandon-the-errored-script-thread recovery: frames here are being discarded by
// design. It is still a lie about the guest's real control flow, so it is OFF by
// default and every skip prints; a run with skips is a DIAGNOSTIC run whose job is
// to name the addresses, not a run whose success means anything.
//
// The hunt does not accept any lifted address it finds on the stack — a guest-stack
// scan yields plausible dwords, not frames (§9 of the guide). A return address has
// one property that stale residue usually lacks: the bytes just before it decode as
// a call. Check the three encodings that matter (E8 rel32, FF /2 reg, FF 15 disp32)
// the same way tools/unwind_candidates.py does offline.
static bool unwind_is_return_site(uint32_t va) {
    if (win32_mem_read8(va - 5) == 0xE8) return true;                  // call rel32
    uint8_t m2 = win32_mem_read8(va - 2);
    if (m2 == 0xFF) {                                                  // call r/m32, mod=11
        uint8_t modrm = win32_mem_read8(va - 1);
        if ((modrm >> 6) == 3 && ((modrm >> 3) & 7) == 2) return true;
    }
    if (win32_mem_read8(va - 6) == 0xFF && win32_mem_read8(va - 5) == 0x15)
        return true;                                                   // call [disp32]
    return false;
}

// Returns true and rewrites (pc, esp) to the next climbable frame, or false if the
// stack holds none below `root`.
static bool unwind_skip_to_next(State& st, uint32_t root, uint32_t* pc) {
    for (uint32_t ad = st.gpr.rsp.dword; ad < root; ad += 4) {
        uint32_t rv = win32_mem_read32(ad);
        if (rv < 0x401000u || rv >= 0xa31000u) continue;
        if (!win32_lifted_has(rv) || !unwind_is_return_site(rv)) continue;
        st.gpr.rsp.dword = ad + 4;
        *pc = rv;
        return true;
    }
    return false;
}


// 🎯🎯🎯 IS THIS ADDRESS A GENUINE RETURN ADDRESS?
//
// The guest-return trampoline below climbs the guest return chain after a
// longjmp by re-dispatching at each address the guest `ret`s to. That is sound
// ONLY for real return addresses. A word on the stack that merely looks like
// .text is not one, and re-dispatching there enters the middle of a function
// with whatever registers the longjmp left — no prologue has run, so anything
// the function established in esi/edi/ebx is foreign.
//
// Measured consequence: tid 12 gets re-dispatched at 0x883440, the LOOP HEAD of
// 0x8833e0. That block expects esi = the GSC VM instance and edi = esi*0x42a8,
// both set by a prologue that never ran; it therefore calls the VM with
// esi = 1043, and the campaign dies on a false "exceeded maximum number of child
// client script variables". 0x883440 is preceded by `lea ebx,[ebx]` padding, not
// by a call.
//
// tools/unwind_candidates.py ALREADY applies exactly this rule when choosing
// which VAs to lift ("survive the 'preceded by a call' check"); it simply was
// never applied to the climb itself. Same rule, same place it matters.
//
// Encodings accepted, matching that script's 2..7 byte window:
//   E8 rel32                     (5)  direct call
//   FF /2  with any modrm/sib    (2..7) indirect call
//   9A ptr16:32                  (7)  far call
// Fin de .text de la cible : 0xa31000 est celle de t6sp (0xa314b0 arrondi).
// Les boucles de saut et la reprise non locale en dependent.
#if defined(BO2_TARGET_SPS)
static constexpr uint32_t kTexteFin = 0x9ef000u;   // .text Steam 0x401000..0x9e7445 + .interpr (boucles Lua) ..0x9ee62c
#elif defined(BO2_TARGET_MPS)
static constexpr uint32_t kTexteFin = 0xb09000u;   // multi Steam : .text ..0xb01f45 + .interpr 0xb02000..0xb0862c
#else
static constexpr uint32_t kTexteFin = 0xb07000u;   // Zombies Steam : .text ..0xaff245 + .interpr 0xb00000..0xb0662c
#endif
static bool unwind_ret_is_call_preceded(uint32_t va) {
    if (va < 0x401000u || va >= kTexteFin) return false;
    if (win32_mem_read8(va - 5u) == 0xE8u) return true;          // call rel32
    for (uint32_t len = 2; len <= 7; len++) {
        const uint32_t st_ = va - len;
        if (win32_mem_read8(st_) == 0xFFu) {
            const uint8_t modrm = win32_mem_read8(st_ + 1u);
            if (((modrm >> 3) & 7u) == 2u) return true;          // /2 = call r/m32
        }
        if (len == 7 && win32_mem_read8(st_) == 0x9Au) return true;
    }
    return false;
}

// P708 (Zombies), P725 (t6sp, crochets du code du tas) : RETOUR NON LOCAL. Un `ret` invite vers une adresse qu'aucun `call` ne precede
// (saut « push ; ret » du CEG, « retour+3 » de son reparateur, code dechiffre dans le
// tas) ne revient pas a l'appelant. Sous Windows le processeur y va ; ici l'appelant
// lifte reprendrait apres son `call`, et le CEG ne remplit jamais le trou qu'il a creuse
// dans le cadre (0x42e4c0 rendait ensuite vers 0xa415b9de). On fait ce que fait
// bo2_longjmp : abandonner les cadres hotes du fil, reprendre a la racine de dispatch,
// a la vraie cible ; la boucle de sauts suit ensuite chaque `ret` par la pile invitee
// (win32_run_lifted, code du tas compris). Les `REPRISE-RACINE` sont les adresses a
// lifter pour que les sauts remontent (recoltees par scripts/mp_iterate.sh).
extern "C" void bo2_reprise_non_locale(State& st, uint32_t pc) {
    GThread* t = g_threads[g_cur];
    static int dits[64];
    const int k = (g_cur >= 0 && g_cur < 64) ? (int)g_cur : 63;
    if (!t->dispatch_armed) {
        if (dits[k]++ < 4)
            fprintf(stderr, "[REPRISE] tid=%d ret -> 0x%08x : aucune racine armee, retour ignore\n", (int)g_cur, pc);
        return;
    }
    if (dits[k]++ < 48) {
        const uint32_t esp = st.gpr.rsp.dword;
        fprintf(stderr, "[REPRISE] tid=%d ret -> 0x%08x esp=0x%08x ebp=0x%08x eax=0x%08x%s\n", (int)g_cur, pc, esp,
                st.gpr.rbp.dword, st.gpr.rax.dword,
                pc >= 0x401000u && pc < kTexteFin && !win32_lifted_has(pc) ? " (cible NON liftee)" : "");
        if (pc >= 0x401000u && pc < kTexteFin && !win32_lifted_has(pc)) fprintf(stderr, "REPRISE-RACINE %#x\n", pc);
        int n = 0;
        for (uint32_t a = esp; a < esp + 0x2000u && n < 24; a += 4) {
            const uint32_t rv = win32_mem_read32(a);
            if (unwind_ret_is_call_preceded(rv) && !win32_lifted_has(rv)) { fprintf(stderr, "REPRISE-RACINE %#x\n", rv); n++; }
        }
    }
    t->resume_pc = pc;
    t->unwind_env = 0;
    _longjmp(t->dispatch_jb, 1);                      // abandonne les cadres hotes ; ne revient pas
}

static void thread_trampoline() {
    GThread* t = g_threads[g_cur];
    State st; std::memset(&st, 0, sizeof(st));
    // FPU defaults: a zeroed control word means "all FP exceptions UNMASKED" to
    // CRT code that fnstcw/stmxcsr-checks it (the trunc helper 0x95f1e0 raised
    // STATUS_FLOAT_INEXACT once the render loop hit it). Match FNINIT/Windows.
    st.x87.fxsave.cwd.flat = 0x027F;
    st.x87.fxsave.mxcsr.flat = 0x1F80;
    t->cur_state = &st;                            // expose to the watchdog
    // HONOUR dwStackSize, WITH A 1 MB FLOOR.
    // Every guest thread used to get a hardcoded 0x40000 (256 KB) and the
    // dwStackSize argument was discarded in impl_CreateThread. Windows gives a
    // thread max(dwStackSize, the PE's SizeOfStackReserve), which for this image
    // is 1 MB — so the port was handing every worker a quarter of what the game
    // asked for, and a guest stack overflow here is silent: it walks straight
    // into whatever guest_alloc handed out below it. Measured consequence (see
    // docs/handoff/SP-01): tid 2's frames were found sitting on top of the
    // lock-free job-node pool, which is what stops the campaign zone load.
    uint32_t stksz = t->req_stack > 0x100000u ? t->req_stack : 0x100000u;
    stksz = (stksz + 0xfffu) & ~0xfffu;
    // Z-NATIF-12e : une case reprise (spawn_thread) arrive avec la pile, le TEB, le tableau TLS et le bloc TLS
    // statique du fil qui y a fini. Windows donne des pages a zero a un fil neuf : pile et TEB repris le sont aussi.
    const bool repris = t->guest_stack && t->guest_stack_size == stksz;
    if (t->guest_stack && !repris) win32_guest_free(t->guest_stack);   // autre taille demandee : pile neuve
    uint32_t stk = repris ? t->guest_stack : win32_guest_alloc(stksz, 16);
    if (repris) std::memset(win32_guest_ptr(stk), 0, stksz);
    // win32_guest_alloc rend 0 sur arene pleine, sur debordement d'alignement et
    // sur collision avec le trou du TEB (kernel32_impl.cpp:161/168/170). Sans ce
    // test, `esp = 0 + stksz - 0x20` posait la pile du thread en mémoire basse et
    // le defaut se presentait des centaines de milliers d'instructions plus loin,
    // sous un visage qui n'a rien a voir. Le depot prefere un maillon qui hurle a
    // un maillon qui echoue sans rien arreter.
    if (!stk) {
        fprintf(stderr, "[thread] ⛔ FATAL : arene invitee pleine — pile de %u o "
                        "refusee pour proc=0x%08x (tid %d). Threads vivants : %zu.\n",
                stksz, t->proc, (int)g_cur, sched_nthreads());
        abort();
    }
    uint32_t esp = stk + stksz - 0x20;
    t->guest_stack_size = stksz;
    // Per-thread TEB. The game caches GetCurrentThreadId in a struct reached
    // DIRECTLY through fs:[0x2c] (0x80df30 / 0x502ff0: "am I the thread in
    // [0x2925588]?"), and the CRT keeps its ptd via Tls*Value. With ONE shared
    // TEB every guest thread saw MAIN's cached tid, so tid4 (render thread)
    // took main's R_WaitBackend path (0x697b00, spins on [0x338f85c]) instead
    // of reaching the backend loop FUN_006cac40 that SETS that flag — the
    // whole frame pipeline self-deadlocked. TLS array is per-thread; the PEB
    // at 0x7FFD4000 stays shared.
    uint32_t teb    = t->guest_teb ? t->guest_teb : win32_guest_alloc(0x100, 16);
    uint32_t tlsarr = t->tls_va ? t->tls_va : win32_guest_alloc(64 * 4, 4);
    if (t->guest_teb) std::memset(win32_guest_ptr(teb), 0, 0x100);
    if (t->guest_tls_blk) pe_tls_fill(t->guest_tls_blk);
    else t->guest_tls_blk = pe_tls_new_block();
    t->guest_teb = teb;
    for (int i = 0; i < 64; i++) win32_mem_write32(tlsarr + i * 4, 0);
    win32_mem_write32(teb + 0x00, 0xFFFFFFFFu);    // SEH chain terminator
    win32_mem_write32(teb + 0x04, stk + stksz);    // stack base
    win32_mem_write32(teb + 0x08, stk);            // stack limit
    win32_mem_write32(teb + 0x18, teb);            // TEB self-pointer
    win32_mem_write32(teb + 0x2C, tlsarr);         // ThreadLocalStoragePointer
    win32_mem_write32(teb + 0x30, 0x7FFD4000u);    // PEB (shared)
    win32_mem_write32(tlsarr + 0, t->guest_tls_blk);    // static-TLS block, index 0
    t->tls_va = tlsarr;
    t->guest_stack = stk;
    st.addr.fs_base.dword = teb;
    win32_mem_write32(esp + 4, t->param);
    win32_mem_write32(esp + 0, 0xDEAD0000u);
    st.gpr.rsp.dword = esp;
    // Seed EBP to the thread's stack base so any guest function that does
    // `push ebp; mov ebp, esp; and esp, -8; ...; mov esp, ebp; pop ebp; ret`
    // has a sane fallback if its caller stomped EBP. The worker procs (e.g.
    // sub_80dee0) skip their own prologue, so EBP would otherwise stay 0
    // from thread start until the first frame that sets it.
    st.gpr.rbp.dword = esp;
    fprintf(stderr, "[thread] ENTER proc=0x%08x param=0x%08x (tid %d) stk=0x%08x esp=0x%08x ebp=0x%08x\n",
            t->proc, t->param, (int)g_cur, stk, esp, esp);
    // Dispatch root for guest longjmp (Lua lua_pcall/luaD_throw). bo2_longjmp
    // restores guest regs from the jmpbuf, sets resume_pc, and host-longjmps
    // here; we then re-dispatch at the guest setjmp-return EIP with the restored
    // guest ESP — the host C++ frames of the failed protected call are unwound.
    // Dispatch root for guest longjmp (Lua lua_pcall/luaD_throw). bo2_longjmp
    // restores guest regs from the jmpbuf, sets resume_pc, and host-longjmps
    // here; we re-dispatch at the guest setjmp-return EIP with the restored
    // guest ESP — the host C++ frames of the failed protected call are unwound.
    // BO2_UNWIND_LOG=1 — these lines used to print UNCONDITIONALLY, so a run with
    // no BO2_* variable at all emitted ~5500 probe lines (1364 longjmps × 4 hops)
    // and violated the guide's §3 invariant. They are diagnostics, not events:
    // a Lua error unwound through a protected call is the VM working as designed.
    // The genuine failures below ("NO LIFTED BLOCK", "hop cap") stay unconditional.
    static const bool unwind_log = bo2_getenv("BO2_UNWIND_LOG") != nullptr;
    if (setjmp(t->dispatch_jb) != 0) {
        if (unwind_log) {
            fprintf(stderr, "[longjmp] tid=%d resume pc=0x%08x eax=0x%08x esp=0x%08x\n",
                    (int)g_cur, t->resume_pc, st.gpr.rax.dword, st.gpr.rsp.dword);
            // Name the error, a bounded number of times: the whole point is that
            // the storm is ONE error repeated, so the first few answer it.
            static int named = 0;
            // Gate on the KNOWN resume point: the L-from-jmpbuf derivation is only
            // valid for sub_6404b0's frame (resume 0x6404e0). Any other setjmp
            // site has a different layout and would produce confident nonsense.
            if (named < 12 && t->resume_pc == 0x6404e0u) {
                named++; bo2_lua_report_error("longjmp", t->unwind_env);
            }
        }
        t->unwinding = true;                        // walk the guest chain back up
    }
    t->dispatch_armed = true;
    // Guest-return trampoline. Normally one win32_run_lifted call runs the whole
    // thread (the guest call tree nests inside host frames). After a longjmp the
    // host frames are gone, so when the resumed guest function `ret`s we land back
    // here instead of in its guest caller — but the address it popped is exactly
    // where execution must continue. Re-dispatch there, repeatedly, until the guest
    // ESP passes the thread's initial stack frame (= the thread proc itself
    // returned). This is what keeps a render-pool worker ALIVE through a Lua error:
    // it climbs back into its job loop instead of ending the thread.
    {
        uint32_t pc = t->resume_pc ? t->resume_pc : t->proc;
        t->resume_pc = 0;
        for (int hop = 0; ; hop++) {
            st._0 = 0;                              // C41 : le dernier `ret` de CE fil (P644)
            // 🎯 CHECK THE ADDRESS WE ARE ABOUT TO RE-ENTER, INCLUDING hop 0.
            // The first pc comes from t->resume_pc (set by bo2_longjmp from the
            // jmpbuf's eip), not from g_last_ret_pc, so a guard placed only on
            // the hop tail misses it entirely — measured: the run still died with
            // esi=1043 and the tail guard never fired.
            if (hop > 0 || pc != t->proc) {
                const bool real = unwind_ret_is_call_preceded(pc);
                static int bad_entry = 0;
                if (!real && bad_entry < 12) {
                    ++bad_entry;
                    fprintf(stderr, "[unwind-badret] tid=%d hop %d ENTER 0x%08x is NOT preceded "
                            "by a call — mid-function entry with the post-longjmp register file "
                            "(esi=%08x edi=%08x ebx=%08x ebp=%08x esp=%08x)%s\n",
                            (int)g_cur, hop, pc, st.gpr.rsi.dword, st.gpr.rdi.dword,
                            st.gpr.rbx.dword, st.gpr.rbp.dword, st.gpr.rsp.dword,
                            bo2_getenv("BO2_UNWIND_STRICT") ? "  -> SKIPPED (BO2_UNWIND_STRICT)" : "");
                }
                static const bool strict_e = bo2_getenv("BO2_UNWIND_STRICT") != nullptr;
                if (strict_e && !real) break;
            }
            if (!win32_run_lifted(&st, pc)) {
                fprintf(stderr, "[unwind] tid=%d NO LIFTED BLOCK at 0x%08x (hop %d) — "
                        "thread ends; lift this VA to climb further\n", (int)g_cur, pc, hop);
                // ONE RUN PER HOP IS NOT A PLAN. Each fix here costs a six-minute
                // boot, and the climb has moved three times already. So dump the
                // rest of the candidate chain at once: every .text-looking dword
                // still on this thread's stack that has NO lifted block.
                //
                // ⚠️ THIS IS A LIST OF LIFT TARGETS, NOT A CALL CHAIN. §9 of the
                // guide is explicit that a guest-stack scan yields plausible
                // addresses, not frames, and a chain derived from one is fiction.
                // Nothing here is used to conclude anything: the addresses are fed
                // to tools/unwind_candidates.py, which KEEPS ONLY those actually
                // preceded by a call instruction, and lifting a VA the thread never
                // reaches costs a few seconds and nothing else.
                fprintf(stderr, "[unwind-cand] tid=%d esp=0x%08x unlifted .text dwords above it:",
                        (int)g_cur, st.gpr.rsp.dword);
                int shown = 0;
                for (uint32_t ad = st.gpr.rsp.dword; ad < esp && shown < 64; ad += 4) {
                    uint32_t rv = win32_mem_read32(ad);
                    if (rv >= 0x401000u && rv < kTexteFin && !win32_lifted_has(rv)) {
                        fprintf(stderr, " %08x", rv); shown++;
                    }
                }
                fprintf(stderr, "\n");
                static const bool skip = bo2_getenv("BO2_UNWIND_SKIP") != nullptr;
                static int skips = 0;
                uint32_t next = 0;
                if (skip && skips < 32 && unwind_skip_to_next(st, esp, &next)) {
                    fprintf(stderr, "[unwind-skip] tid=%d LIFT 0x%08x — resuming at 0x%08x "
                            "(skip %d; frames in between ABANDONED, this run is diagnostic)\n",
                            (int)g_cur, pc, next, ++skips);
                    pc = next;
                    continue;
                }
                break;
            }
            if (!t->unwinding) break;               // never longjmp'd: proc returned
            uint32_t rp = st._0;                    // C41 : pas le ret d'un fil parallele (P644)
            if (st.gpr.rsp.dword > esp || rp == 0 || rp == 0xDEAD0000u) {
                if (unwind_log)
                    fprintf(stderr, "[unwind] tid=%d reached thread root (esp=0x%08x ret=0x%08x)\n",
                            (int)g_cur, st.gpr.rsp.dword, rp);
                break;
            }
            if (hop >= 256) {                       // guard: a mis-read chain, not a real one
                fprintf(stderr, "[unwind] tid=%d hop cap at 0x%08x — giving up\n", (int)g_cur, rp);
                break;
            }
            // 🎯 THE CLIMB MUST ONLY LAND ON REAL RETURN ADDRESSES.
            // Always evaluated, always reported when it fails — a hop onto a
            // non-call-preceded address is the defect that puts esi=1043 into
            // the GSC VM, and it used to be completely silent (a successful hop
            // logs nothing under BO2_UNWIND_LOG).
            {
                const bool real = unwind_ret_is_call_preceded(rp);
                static int bad_hops = 0;
                if (!real && bad_hops < 8) {
                    ++bad_hops;
                    {   // 🎯 WHAT IS ON THE NATIVE STACK where the bad return address came from:
                        // the fatal throw is byte-identical to 17,849 healthy ones, so the
                        // jmpbuf is not the defect -- the frame the VM returns through is.
                        // 16 guest words from esp-8 upward, plus the jmpbuf frame's [ebp..ebp+8].
                        const uint32_t sp = st.gpr.rsp.dword, bp = st.gpr.rbp.dword;
                        fprintf(stderr, "[unwind-badret]   native stack at esp=0x%08x:", sp);
                        for (int w = -2; w < 14; w++) fprintf(stderr, " %08x", win32_mem_read32(sp + (uint32_t)(w * 4)));
                        fprintf(stderr, "\n[unwind-badret]   ebp=0x%08x [ebp]=%08x [ebp+4]=%08x [ebp+8]=%08x\n",
                                bp, win32_mem_read32(bp), win32_mem_read32(bp + 4u), win32_mem_read32(bp + 8u));
                    }
                    if (g_cur >= 0 && g_cur < 64) {
                        const uint32_t n = g_lj_n[g_cur];
                        if (n) {
                            const LjRec& last = g_lj_ring[g_cur][(n - 1u) & 3u];
                            fprintf(stderr, "[unwind-frame] tid=%d env=%08x raw24=", (int)g_cur, last.env);
                            for (uint32_t j = 0; j < 24; ++j)
                                fprintf(stderr, "%02x", (unsigned)win32_mem_read8(last.env + j));
                            fprintf(stderr, "\n");
                            // Use the saved frame pointer, not the damaged live EBP.
                            // VM prologue: push ebp; lea ebp,[esp-0x74].
                            // Its saved EBP is at +0x74, return at +0x78.
                            // The jmpbuf EBP may itself be overwritten. At this exact VM
                            // setjmp site, the verified prologue gives EBP = saved ESP + 0x11c.
                            const uint32_t frame_bp = last.jeip == 0x87f852u ? last.jesp + 0x11cu : last.jebp;
                            const uint32_t begin = frame_bp - 0x130u;
                            for (uint32_t row = 0; row < 0x1c0u; row += 16u) {
                                const uint32_t a = begin + row;
                                fprintf(stderr, "[unwind-frame] %08x: %08x %08x %08x %08x\n",
                                        a, win32_mem_read32(a), win32_mem_read32(a + 4u),
                                        win32_mem_read32(a + 8u), win32_mem_read32(a + 12u));
                            }
                            fprintf(stderr, "[unwind-frame] jmpbuf-ebp=%08x vm-return-slot=%08x value=%08x live-ebp=%08x live-esp=%08x\n",
                                    last.jebp, frame_bp + 0x78u, win32_mem_read32(frame_bp + 0x78u),
                                    st.gpr.rbp.dword, st.gpr.rsp.dword);
                        }
                        for (uint32_t k = (n > 4 ? n - 4 : 0); k < n; k++) { const LjRec& r = g_lj_ring[g_cur][k & 3u];
                            fprintf(stderr, "[unwind-badret]   throw #%u on this tid: env=0x%08x from=0x%08x esp=0x%08x | jb esp=0x%08x eip=0x%08x ebp=0x%08x ebx=%08x edi=%08x esi=%08x | live ebp at its setjmp #%llu = 0x%08x%s\n",
                                    k + 1, r.env, r.from, r.esp, r.jesp, r.jeip, r.jebp, r.jebx, r.jedi, r.jesi, (unsigned long long)r.sjn, r.sjebp, (r.esp > r.jesp) ? "  <- STALE (setjmp frame already returned)" : ""); }
                    }
                    fprintf(stderr, "[unwind-badret] tid=%d hop %d -> 0x%08x is NOT preceded by "
                            "a call — re-dispatching there enters mid-function with the "
                            "post-longjmp register file (esi=%08x edi=%08x ebx=%08x ebp=%08x). "
                            "esp=0x%08x%s\n",
                            (int)g_cur, hop, rp, st.gpr.rsi.dword, st.gpr.rdi.dword,
                            st.gpr.rbx.dword, st.gpr.rbp.dword, st.gpr.rsp.dword,
                            bo2_getenv("BO2_UNWIND_STRICT") ? "  -> SKIPPED (BO2_UNWIND_STRICT)" : "");
                }
                // BO2_UNWIND_STRICT: treat a non-return address as the top of the
                // climb rather than walking into it. Off by default until A/B'd.
                static const bool strict = bo2_getenv("BO2_UNWIND_STRICT") != nullptr;
                if (strict && !real) break;
            }
            if (unwind_log)
                fprintf(stderr, "[unwind] tid=%d hop %d -> 0x%08x (esp=0x%08x)\n",
                        (int)g_cur, hop, rp, st.gpr.rsp.dword);
            pc = rp;
        }
    }
    win32_report_orphaned_events(g_cur);
    fprintf(stderr, "[thread] DONE proc=0x%08x (tid %d)\n", t->proc, (int)g_cur);
    t->status = T_DONE;
    if (t->hosted) return;      // fil hote : on sort, le systeme joint le thread
    for (;;) coop_yield();                          // never resumes (status DONE)
}

// Run the MAIN thread's guest entry with the SAME dispatch-root + guest-return
// unwind as thread_trampoline. Without this, a guest longjmp on main (the GSC
// script VM's error recovery: Scr_Error -> 0x87e0a0 -> longjmp(env,-1) resuming
// at the VM's setjmp point 0x87f852) is UNARMED: bo2_longjmp bails, the lifted
// longjmp falls through, and the VM re-enters at a null state and hangs.
//
// longjmp only rewinds guest esp to the setjmp frame; every guest return address
// ABOVE that point is still on the guest stack in g_mem, so re-dispatching at the
// setjmp-return PC and then climbing the guest return chain walks main's real
// call stack (VM_Execute -> its caller -> ... -> the frame driver) back up. The
// game keeps running: the errored script thread is abandoned, the frame loop
// continues. main() calls this instead of the entry directly.
// Z-NATIF-08y : BO2_MEMDUMP=<prefixe> ecrit <prefixe>.image (l'image mappee, jusqu'a la fin de .rsrc) et
// <prefixe>.pile (2 pages autour d'esp) quand le fil principal s'arrete sur une cible non traduite. Sert a rejouer
// hors ligne, sur les octets d'origine et l'etat reel, le controle qui vient d'echouer.
// Diagnostic seul : rien n'est modifie.
static void vider_memoire(uint32_t esp) {
    const char* prefixe = bo2_getenv("BO2_MEMDUMP");
    if (!prefixe || !*prefixe) return;
    const uint32_t page = esp & ~0xfffu;
    const struct { const char* suffixe; uint32_t lo, hi; } plages[] = {{"image", 0x400000u, 0x43bc000u},
                                                                       {"pile", page - 0x1000u, page + 0x1000u}};
    for (const auto& pl : plages) {
        char chemin[1024];
        snprintf(chemin, sizeof chemin, "%s.%s", prefixe, pl.suffixe);
        FILE* f = fopen(chemin, "wb");
        const size_t ecrit = f ? fwrite(win32_guest_ptr(pl.lo), 1, pl.hi - pl.lo, f) : 0;
        if (f) fclose(f);
        fprintf(stderr, "[MEMDUMP] %s : 0x%08x..0x%08x esp=0x%08x, %zu octets\n", chemin, pl.lo, pl.hi, esp, ecrit);
    }
}

static void win32_run_main_thread_impl(State* st, uint32_t entry) {
    sched_init();
    g_cur = 0;
    GThread* t = g_threads[0];
    t->cur_state = st;
    t->proc = entry;
    uint32_t esp0 = st->gpr.rsp.dword;              // initial guest sp (frame root)
    static int s_main_lj = 0;
    if (setjmp(t->dispatch_jb) != 0) {
        if (++s_main_lj <= 8 || (s_main_lj % 1000) == 0)
            fprintf(stderr, "[longjmp] tid=0 (main) #%d resume pc=0x%08x eax=0x%08x esp=0x%08x\n",
                    s_main_lj, t->resume_pc, st->gpr.rax.dword, st->gpr.rsp.dword);
        if (s_main_lj == 1) win32_gsc_dump_ring();   // snapshot early, before the loop muddies it
        t->unwinding = true;
    }
    t->dispatch_armed = true;
    uint32_t pc = t->resume_pc ? t->resume_pc : entry;
    t->resume_pc = 0;
    for (int hop = 0; ; hop++) {
        st->_0 = 0;                                 // C41 : le dernier `ret` vit dans la State
        if (!win32_run_lifted(st, pc)) {
            fprintf(stderr, "[unwind] tid=0 NO LIFTED BLOCK at 0x%08x (hop %d)\n", pc, hop);
            vider_memoire(st->gpr.rsp.dword);
            // Same reasoning as the worker-thread path above, and learned the same
            // expensive way: this climb has now moved three times (0x883679 ->
            // 0x469b8a -> 0x630d7d), one five-minute run per hop, because only the
            // address that happened to be next was ever printed. Dump the WHOLE
            // remaining candidate chain so a single run names every hop left.
            // ⚠️ A LIST OF LIFT TARGETS, NOT A CALL CHAIN — a guest-stack scan
            // yields plausible addresses, not frames. tools/unwind_candidates.py
            // keeps only those actually preceded by a call; lifting one the thread
            // never reaches costs ~50ms and nothing else.
            fprintf(stderr, "[unwind-cand] tid=0 esp=0x%08x unlifted .text dwords above it:",
                    st->gpr.rsp.dword);
            int shown = 0;
            for (uint32_t ad = st->gpr.rsp.dword; ad < esp0 && shown < 64; ad += 4) {
                uint32_t rv = win32_mem_read32(ad);
                if (rv >= 0x401000u && rv < kTexteFin && !win32_lifted_has(rv)) {
                    fprintf(stderr, " %08x", rv); shown++;
                }
            }
            fprintf(stderr, "\n");
            // BO2_UNWIND_SKIP — see unwind_skip_to_next above. One diagnostic run
            // then names EVERY missing hop instead of one boot per hop.
            static const bool skip = bo2_getenv("BO2_UNWIND_SKIP") != nullptr;
            static int skips = 0;
            uint32_t next = 0;
            if (skip && skips < 32 && unwind_skip_to_next(*st, esp0, &next)) {
                fprintf(stderr, "[unwind-skip] tid=0 LIFT 0x%08x — resuming at 0x%08x "
                        "(skip %d; frames in between ABANDONED, this run is diagnostic)\n",
                        pc, next, ++skips);
                pc = next;
                continue;
            }
            break;
        }
        if (!t->unwinding) break;                   // normal return: entry finished
        uint32_t rp = st->_0;
        // Sentinels: main's initial stack held 0xDEADDEAD as the entry return
        // address; a guest esp past the initial sp means the entry itself
        // returned. Either way the whole program is done.
        if (st->gpr.rsp.dword > esp0 || rp == 0 || rp == 0xDEADDEADu || rp == 0xDEAD0000u) {
            fprintf(stderr, "[unwind] tid=0 reached program root (esp=0x%08x ret=0x%08x)\n",
                    st->gpr.rsp.dword, rp);
            break;
        }
        if (hop >= 512) { fprintf(stderr, "[unwind] tid=0 hop cap at 0x%08x\n", rp); break; }
        pc = rp;
    }
}

// The whole guest — main (tid 0) AND every ucontext coroutine it spawns — runs on
// ONE host thread via swapcontext. main's GSC menu-init recurses far deeper than
// the 8 MB default host stack (a burst peaked >8 MB and SIGSEGV'd in the lifted
// dispatch, guest ESP flat = host recursion, not guest depth). ucontext coroutines
// already get their own COSTACK; main was the exception (hstack=null → real main
// stack). Drive the whole scheduler from a dedicated large-stack host thread so
// main gets the same headroom. The crash handler is armed HERE (sigaltstack is
// per-thread) so an overflow on this thread still dumps the crumb/GSC rings.
struct MainRunArgs { State* st; uint32_t entry; };
// Set when the guest scheduler returns, so the main thread can poll instead of
// blocking in join (macOS has no pthread_tryjoin_np) while it pumps the window.
static std::atomic<bool> g_guest_finished{false};
// ── 🎯 [SPIN] — NAME THE FUNCTION A WEDGED GUEST IS SPINNING IN ──────────────
//
// This is the instrument this port was missing, and its absence has cost whole
// sessions. Every guest thread is a ucontext coroutine on ONE host scheduler
// thread, so a guest thread that loops inside a single lifted block calls no
// import, takes no indirect branch, and never yields — which makes it invisible
// to the import counter, to the dispatch crumb ring, to every interpose, and to
// the GSC opcode ring at once. Two angola freezes were diagnosed only by running
// macOS `sample` by hand and reading its call graph:
//
//     freeze A   1982/2009 samples in sub_87bd70   (MT allocator treap insert)
//     freeze B    730/730  samples in sub_80dee0 -> sub_7fa710 + sub_4a98c0
//
// The same answer is one signal away from being a log line. SIGPROF is delivered
// to the scheduler thread; the handler captures its interrupted PC; the caller
// symbolises it with dladdr on ITS own thread, where doing so is safe.
//
// Capture the interrupted PC from the kernel-provided context. Do not unwind:
// backtrace() in SIGPROF crashed inside __thread_stack_pcs on a coroutine stack
// (astra_angola_current_1, 78df8ad91046a524). Symbolisation stays on the caller.
static pthread_t         g_sched_thread;
static std::atomic<bool> g_sched_thread_valid{false};
static std::atomic<void*> g_spin_pc{nullptr};
static std::atomic<int>  g_spin_n{-1};
static_assert(std::atomic<int>::is_always_lock_free && std::atomic<void*>::is_always_lock_free, "signal publication must be lock-free");
static void spin_sigprof(int, siginfo_t*, void* context) {
    auto* uc = static_cast<ucontext_t*>(context);
    g_spin_pc.store(reinterpret_cast<void*>(BO2_UC_PC(uc)),
                    std::memory_order_relaxed);
    g_spin_n.store(1, std::memory_order_release);
}
// ── 🎯 BO2_SPIN_HIST=1 — RAW PC HISTOGRAM, NO SYMBOL NAMES AT ALL ───────────
//
// Established 2026-08-29: on this Remill-lifted binary EVERY pc-to-name mapping
// is unreliable. `sample` attributes to the nearest preceding symbol and the
// lifted functions are enormous and adjacent — it put 619 samples in a function
// an interposed counter proved had stopped. Mach-O here carries no symbol sizes
// (0 for all 83,669), so a size-aware resolver cannot be built either.
//
// So do not ask for a name. Fire SIGPROF many times, histogram the RAW innermost
// address, and print the top few. One address can then be resolved by hand,
// carefully, once — instead of trusting a resolver for thousands.
extern "C" void win32_spin_histogram(const char* why) {
    if (!bo2_getenv("BO2_SPIN_HIST")) return;
    if (!g_sched_thread_valid.load(std::memory_order_acquire)) {
        fprintf(stderr, "[SPIN-HIST] no guest scheduler thread — NOT LOOKED AT\n");
        return;
    }
    enum { N = 400, SLOTS = 24 };
    void*    pc[SLOTS] = {nullptr};
    unsigned ct[SLOTS] = {0};
    int used = 0, taken = 0;
    for (int k = 0; k < N; k++) {
        g_spin_n.store(-1, std::memory_order_release);
        if (pthread_kill(g_sched_thread, SIGPROF) != 0) break;
        for (int i = 0; i < 400 && g_spin_n.load(std::memory_order_acquire) < 0; i++) usleep(50);
        const int n = g_spin_n.load(std::memory_order_acquire);
        if (n != 1) continue;
        void* a = g_spin_pc.load(std::memory_order_relaxed);
        ++taken;
        int i = 0;
        for (; i < used; i++) if (pc[i] == a) { ++ct[i]; break; }
        if (i == used && used < SLOTS) { pc[used] = a; ct[used++] = 1; }
        usleep(300);
    }
    if (!taken) { fprintf(stderr, "[SPIN-HIST] (%s) no usable sample\n", why); return; }
    fprintf(stderr, "[SPIN-HIST] (%s) %d samples of the guest thread's innermost frame, "
                    "RAW addresses — resolve ONE by hand, do not trust a symbol table here:\n",
            why, taken);
    for (int r = 0; r < 6; r++) {
        int best = -1;
        for (int i = 0; i < used; i++) if (ct[i] && (best < 0 || ct[i] > ct[best])) best = i;
        if (best < 0) break;
        fprintf(stderr, "[SPIN-HIST]   %5u/%d  %p\n", ct[best], taken, pc[best]);
        ct[best] = 0;
    }
}

// ── 🎯 [GSTACK] — THE GUEST CALL STACK OF THE COROUTINE THAT HOLDS THE THREAD ──
//
// [SPIN] names the HOST function (win32_mem_write32_slow, __remill_read_memory_32
// — always a memory primitive, which says nothing) and the crumb ring only sees
// indirect dispatches. The question every frozen angola run leaves unanswered is
// WHICH GUEST FUNCTION the wedged coroutine is inside, and that is written on its
// guest stack: every native `call` pushed a return address there. Walk it exactly
// as the unwinder does — accept a word only if it is inside .text AND the bytes
// before it decode as a call (unwind_ret_is_call_preceded) — and print the chain.
// Read-only, no locks, guest memory is a flat mapping: safe from the watchdog
// thread while the scheduler runs. A stale ESP only costs a few junk words at the
// bottom of the walk, never a wrong frame: the call-preceded test rejects those.
// ⚠️ The sampler catches whoever is RUNNING, and that is not the culprit: a
// frozen angola shows the sound worker running happily while Main waits. So walk
// EVERY thread, and say what each is blocked on — the answer to "no new frame"
// is on Main's stack, not on the runner's.
static void bo2_guest_stack_of(const char* why, int t) {
    if (t < 0 || (size_t)t >= sched_nthreads() || !g_threads[t]) return;
    GThread* g = g_threads[t];
    State* st = g->cur_state;
    if (!st) { fprintf(stderr, "[GSTACK] (%s) tid=%d has no State yet\n", why, t); return; }
    const uint32_t esp = st->gpr.rsp.dword, ebp = st->gpr.rbp.dword;
    fprintf(stderr, "[GSTACK] (%s) tid=%-2d %s picks=%llu status=%d wait_h=0x%05x wake_at=%llu proc=0x%08x param=0x%x esp=0x%08x ebp=0x%08x last-ret=0x%08x\n",
            why, t, t == (int)g_cur ? "RUNNING" : "       ", (unsigned long long)(t < 32 ? g_pick_count[t] : 0), g->status, g->wait_h,
            (unsigned long long)g->wake_at, g->proc, g->param, esp, ebp, t == (int)g_cur ? g_last_ret_pc : 0u);
    uint32_t prev = 0; int shown = 0, rep = 0;
    for (uint32_t a = esp; a < esp + 0x4000u && shown < 12; a += 4u) {
        const uint32_t rv = win32_mem_read32(a);
        if (rv < 0x401000u || rv >= kTexteFin) continue;   // la fin de .text de la CIBLE : la CRT des Zombies est en 0xa7xxxx
        if (!unwind_ret_is_call_preceded(rv)) continue;
        if (rv == prev) { rep++; continue; }        // a recursive frame repeats: count it, print once
        if (rep) { fprintf(stderr, "[GSTACK]     (x%d more of the same)\n", rep); rep = 0; }
        fprintf(stderr, "[GSTACK]   [esp+0x%04x] ret to 0x%08x%s\n", a - esp, rv,
                win32_lifted_has(rv) ? "" : "   <- NOT LIFTED");
        prev = rv; shown++;
    }
    if (rep) fprintf(stderr, "[GSTACK]     (x%d more of the same)\n", rep);
    if (!shown) fprintf(stderr, "[GSTACK]   nothing call-preceded in 16 KB above esp — the frame is a leaf loop, or esp is stale\n");
}

// ── 🎯 win32_guest_caller_chain — LA CHAINE D'APPEL POUR UNE SONDE D'ECRITURE ──
//
// Une sonde accrochee au CHEMIN D'ECRITURE (sl_selflink_watch, par exemple) n'a
// ni `State*` ni cadre : elle ne peut pas lire `win32_mem_read32(esp)` et
// appeler ca « l'adresse de retour ». Au milieu d'un store quelconque, `[esp]`
// est un LOCAL, pas un retour — c'est mot pour mot le piege du registre de base
// (AGENTS.md §3), celui qui a deja produit trois erreurs de nommage ici.
//
// La seule reponse honnete est celle que [GSTACK] applique deja : ne retenir un
// mot au-dessus d'ESP que s'il est dans `.text` ET que les octets qui le
// precedent decodent comme un `call`. On rend les `max` premiers dans un tampon
// fourni par l'appelant, pour qu'une sonde les mette sur sa propre ligne.
// Lecture seule, pas de verrou, memoire invitee a plat.
//
// ⚠️ TEMOIN NEGATIF OBLIGATOIRE : si rien n'est call-preceded, on l'ECRIT. Une
// chaine vide qui se lit comme « pas d'appelant » est une panne d'instrument
// deguisee en mesure — 41 % des retractations de ce depot sont de ce type.
extern "C" int win32_guest_caller_chain(char* out, size_t n, int max) {
    if (!out || n == 0) return 0;
    out[0] = '\0';
    State* st = win32_current_guest_state();
    if (!st) { snprintf(out, n, "(aucun State invite)"); return 0; }
    const uint32_t esp = st->gpr.rsp.dword;
    size_t w = (size_t)snprintf(out, n, "esp=0x%08x", esp);
    int shown = 0; uint32_t prev = 0;
    for (uint32_t a = esp; a < esp + 0x4000u && shown < max && w + 24u < n; a += 4u) {
        const uint32_t rv = win32_mem_read32(a);
        if (rv < 0x401000u || rv >= 0xa31000u) continue;
        if (!unwind_ret_is_call_preceded(rv)) continue;
        if (rv == prev) continue;                    // un cadre recursif se repete
        w += (size_t)snprintf(out + w, n - w, " <- 0x%08x", rv);
        prev = rv; ++shown;
    }
    if (!shown && w + 48u < n)
        snprintf(out + w, n - w, " <- rien de call-preceded en 16 Ko (temoin negatif)");
    return shown;
}
// ── 🎯 [EBP-DEATH] — CATCH THE FRAME POINTER DYING, AT THE IMPORT THAT FOLLOWS ──
//
// Measured over five consecutive angola runs (runs/sj_1..sj_5), at the terminal
// freeze: Main's EBP is 0x00000000 / 0x00000051 / 0x0000003c / 0x00a3b0f8 while
// its ESP is a perfectly good main-stack pointer. The same shape kills the
// Server (esp=0x7d, ebp=0x2f3bb) — the VM epilogue `lea esp,[ebp+0x74]; pop ebp;
// ret` turns a dead EBP into exactly that. So the defect is NOT server-specific
// and NOT the jmpbuf: _setjmp3 is a leaf that can only copy the live EBP.
//
// A frame pointer is always a stack address in this program (main 0x0010xxxx,
// coroutines 0x4xxxxxxx). Inside the PE image or under 0x10000 it is dead, and
// every [ebp+X] local access from then on lands in low guest memory — which is
// exactly what litters guest 0x1..0x100 and what makes the resolver miss on
// obj=0x0000. Sample it at the import boundary: three loads on a path that
// already costs 3.6 ns per import, and the guest return chain is still intact
// there, so the print names the callee that returned without restoring EBP.
// ── 🎯 [TRAIL] — LES VALEURS DE LA BOUCLE DE sub_78c420 AU GEL (P750, haiti et la_2) ──
//
// Au gel de haiti (mono comme srvbe) et de la_2 (srvbe), un ouvrier jq tourne sans fin dans sub_78c420 :
//     restant = (fin - debut) / periode ; periode = (float)[[ebx+0x100]+8]
//     tant que restant > 1.0 : appel de sub_439300 (alloue un noeud), restant -= pas (1.0 apres le 1er tour)
// Au-dela de 2^24 la soustraction de 1.0 ne change plus restant : la boucle ne finit jamais. Quelle grandeur est
// folle ? On la lit dans la pile invitee, depuis deux adresses de retour :
//   S  = case qui contient 0x78c675 (retour de `call 0x439300` en 0x78c670). Cadre de sub_78c420 F = S + 0x28 :
//        restant [F+0x4c], pas [F+0x58], fin-debut [F+0x08] ; ebx (pousse) [S+0x18].
//   S2 = case qui contient 0x78dd5a (retour de `call 0x78c420` en 0x78dd55) : les 9 arguments en S2+4..S2+0x24,
//        debut = arg7 (S2+0x20), fin = arg8 (S2+0x24).
//   S3 = case qui contient 0x78faff (retour de `call 0x5c1dc0` en 0x78fafa) : effet = arg1 [S3+8]. fin - debut =
//        |[effet+0x5c..0x64] - [effet+0x78..0x80]| (0x78f943), la distance parcourue depuis l'image precedente ;
//        [effet+0xa0] = definition, dont le premier mot pointe sur le nom de l'effet.
// Lecture seule (carte plate : toute adresse 32 bits se lit). Temoin negatif ecrit par l'appelant.
static int bo2_trail_probe(int t, uint32_t esp) {
    uint32_t s = 0, s2 = 0, s3 = 0;
    for (uint32_t a = esp; a < esp + 0x4000u; a += 4u) {
        const uint32_t v = win32_mem_read32(a);
        if (!s && v == 0x0078c675u) s = a;
        if (!s2 && v == 0x0078dd5au) s2 = a;
        if (!s3 && v == 0x0078faffu) s3 = a;
    }
    if (!s && !s2 && !s3) return 0;
    auto f = [](uint32_t a) { const uint32_t w = win32_mem_read32(a); float x; memcpy(&x, &w, 4); return (double)x; };
    if (s) {
        const uint32_t F = s + 0x28u, ed = win32_mem_read32(s + 0x18u), d = win32_mem_read32(ed + 0x100u);
        fprintf(stderr, "[TRAIL] tid=%d S=0x%08x restant=%.9g pas=%.9g fin-debut=%.9g elemDef=0x%08x [+0x100]=0x%08x "
                        "periode=[+8]=%d | [+0x100]+0..0x1c: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                t, s, f(F + 0x4cu), f(F + 0x58u), f(F + 0x08u), ed, d, (int32_t)win32_mem_read32(d + 8u),
                win32_mem_read32(d), win32_mem_read32(d + 4u), win32_mem_read32(d + 8u), win32_mem_read32(d + 0xcu),
                win32_mem_read32(d + 0x10u), win32_mem_read32(d + 0x14u), win32_mem_read32(d + 0x18u),
                win32_mem_read32(d + 0x1cu));
    }
    if (s2) {
        fprintf(stderr, "[TRAIL] tid=%d S2=0x%08x args: %08x %08x %08x %08x %08x %08x %08x | debut=%.9g fin=%.9g\n",
                t, s2, win32_mem_read32(s2 + 4u), win32_mem_read32(s2 + 8u), win32_mem_read32(s2 + 0xcu),
                win32_mem_read32(s2 + 0x10u), win32_mem_read32(s2 + 0x14u), win32_mem_read32(s2 + 0x18u),
                win32_mem_read32(s2 + 0x1cu), f(s2 + 0x20u), f(s2 + 0x24u));
    }
    if (s3) {
        const uint32_t e = win32_mem_read32(s3 + 8u), def = win32_mem_read32(e + 0xa0u);
        char nom[64]; const uint32_t pn = win32_mem_read32(def);
        for (uint32_t i = 0; i < sizeof nom; i++) { nom[i] = (char)win32_mem_read8(pn + i); if (!nom[i]) break; }
        nom[sizeof nom - 1] = '\0';
        fprintf(stderr, "[TRAIL] tid=%d S3=0x%08x effet=0x%08x [+0xfc]=0x%08x origine=(%.9g %.9g %.9g) precedente=(%.9g %.9g %.9g) "
                        "dist[+0xc4]=%.9g [+0xc0]=%d def=0x%08x nom=\"%s\"\n",
                t, s3, e, win32_mem_read32(e + 0xfcu), f(e + 0x5cu), f(e + 0x60u), f(e + 0x64u), f(e + 0x78u),
                f(e + 0x7cu), f(e + 0x80u), f(e + 0xc4u), (int32_t)win32_mem_read32(e + 0xc0u), def, nom);
        // P751 : a quoi l'effet est accroche. 0x78d790 : si (s16)[e+0xf6] >= 0, cadre = objet [[0x2d26fe0]+f6*0x35c]
        // (origine +0x2c, angles +0x38, 0x78d710) compose avec le decalage local de l'effet (quat +0x84) ; si
        // ([e+0xb4]>>8)&3 == 2, cadre = enregistrement de 32 octets de 0x644020(entnum = [e+0xb4]>>10).
        const int16_t f6 = (int16_t)(win32_mem_read32(e + 0xf4u) >> 16);
        const uint32_t b4 = win32_mem_read32(e + 0xb4u);
        fprintf(stderr, "[TRAIL] tid=%d effet=0x%08x [+0xf6]=%d [+0xb4]=0x%08x (type=%u ent=%u) local=(%.9g %.9g %.9g %.9g | %.9g %.9g %.9g)\n",
                t, e, f6, b4, (b4 >> 8) & 3u, b4 >> 10, f(e + 0x84u), f(e + 0x88u), f(e + 0x8cu), f(e + 0x90u),
                f(e + 0x94u), f(e + 0x98u), f(e + 0x9cu));
        if (f6 >= 0) {
            const uint32_t o = win32_mem_read32(0x2d26fe0u) + (uint32_t)f6 * 0x35cu;
            fprintf(stderr, "[TRAIL]   objet[f6]=0x%08x origine=(%.9g %.9g %.9g) angles=(%.9g %.9g %.9g) mots:",
                    o, f(o + 0x2cu), f(o + 0x30u), f(o + 0x34u), f(o + 0x38u), f(o + 0x3cu), f(o + 0x40u));
            for (uint32_t i = 0; i < 0x35cu; i += 4u) fprintf(stderr, " %08x", win32_mem_read32(o + i));
            fprintf(stderr, "\n");
        }
        if (((b4 >> 8) & 3u) == 2u) {
            const uint32_t r = win32_mem_read32(0x27e3490u) + ((b4 >> 10) & 0xffffu) * 32u;
            fprintf(stderr, "[TRAIL]   pose[ent]=0x%08x : %.9g %.9g %.9g %.9g | %.9g %.9g %.9g\n", r, f(r), f(r + 4u),
                    f(r + 8u), f(r + 0xcu), f(r + 0x10u), f(r + 0x14u), f(r + 0x18u));
        }
    }
    return 1;
}
static void cs_lock_report();   // defined with the critical-section shims
static void bo2_guest_stack_of_runner(const char* why) {
    // 🎯 THE ENGINE'S "A Com_Error IS IN PROGRESS" FLAG, 0x0287bb30.
    // sub_444500 (Com_Error — it owns the string "Com_Error called within a task
    // success/failure callback") sets it to 1 at 0x4446fe and clears it only at
    // its own exits (0x44477e, 0x444867) and in the error-recovery path
    // (0x80b93b). EIGHT sites poll it, and the server tick's poll is a hard spin:
    //     0x852b60 cmp [0x287bb30],0 / je out / 0x852b70 Sleep(50) / jne back
    // In runs/vm_2's frozen window that one call site accounts for 1,612,743
    // Sleep(50) calls in six seconds. If a longjmp leaves Com_Error without
    // reaching a clear, the flag stays 1 and every poller waits forever.
    fprintf(stderr, "[ERR-FLAG] (%s) [0x287bb30] = %u   (1 = a Com_Error is still in progress; the server tick at 0x852b60 spins on this)\n",
            why, win32_mem_read32(0x287bb30u));
    fprintf(stderr, "[GSTACK] (%s) ==== %zu guest threads, running tid=%d ====\n", why, sched_nthreads(), (int)g_cur);
    cs_lock_report();
    for (size_t i = 0; i < sched_nthreads(); i++) bo2_guest_stack_of(why, (int)i);
    int trail = 0;   // P750 : [TRAIL], temoin negatif compris
    for (size_t i = 0; i < sched_nthreads(); i++)
        if (g_threads[i] && g_threads[i]->cur_state) trail += bo2_trail_probe((int)i, g_threads[i]->cur_state->gpr.rsp.dword);
    fprintf(stderr, "[TRAIL] (%s) cadres de sub_78c420 / du marcheur / de sub_5c1dc0 trouves sur %d fil(s)\n", why, trail);
    fprintf(stderr, "[GSTACK] ==== end ====\n");
}
extern "C" void bo2_gstack_all(const char* why) { bo2_guest_stack_of_runner(why); }
// [CW] mot de controle x87 et mxcsr de chaque fil invite, une fois au temoin de niveau. P761 : apres le patch remill
// 0003 (FLDCW force le bit 6 reserve), le fil principal doit lire 0x007f et non 0x003f (P759).
extern "C" void bo2_print_thread_cw(const char* why) {
    for (size_t i = 0; i < sched_nthreads(); i++)
        if (g_threads[i] && g_threads[i]->cur_state)
            fprintf(stderr, "[CW] (%s) tid=%zu cw=0x%04x mxcsr=0x%08x\n", why, i,
                    (unsigned)g_threads[i]->cur_state->x87.fxsave.cwd.flat,
                    (unsigned)g_threads[i]->cur_state->x87.fxsave.mxcsr.flat);
}
extern "C" void win32_name_the_spin(const char* why) {
    static const bool on = [] { const char* e = bo2_getenv("BO2_SPINNAME"); return !e || atoi(e) != 0; }();
    if (!on) return;
    if (!g_sched_thread_valid.load(std::memory_order_acquire)) {
        fprintf(stderr, "[SPIN] no guest scheduler thread registered — NOT LOOKED AT\n");
        return;
    }
    g_spin_n.store(-1, std::memory_order_release);
    if (int e = pthread_kill(g_sched_thread, SIGPROF)) {
        // Ne jamais se contenter de « failed » : en fast1 cette ligne est sortie
        // 6 fois au gel et n'a donc nomme aucune spin. ESRCH = le thread de
        // l'ordonnanceur a fini (les coroutines n'ont plus de porteur) ; c'est
        // une information tout a fait differente d'un EINVAL.
        fprintf(stderr, "[SPIN] pthread_kill -> %s (%d) — NOT LOOKED AT%s\n",
                strerror(e), e,
                e == ESRCH ? "   <- le thread hote de l'ordonnanceur a FINI" : "");
        return;
    }
    for (int i = 0; i < 2000 && g_spin_n.load(std::memory_order_acquire) < 0; i++) usleep(200);
    int n = g_spin_n.load(std::memory_order_acquire);
    if (n < 0) {
        fprintf(stderr, "[SPIN] the scheduler thread did not take the signal in 400 ms — "
                        "it is not merely busy, it is not running signal handlers either\n");
        return;
    }
    fprintf(stderr, "[SPIN] (%s) guest scheduler interrupted PC (single sample):\n", why);
    if (n == 1) {
        void* pc = g_spin_pc.load(std::memory_order_relaxed);
        Dl_info di{};
        if (dladdr(pc, &di) && di.dli_sname)
            fprintf(stderr, "[SPIN]   %-32s +0x%lx\n", di.dli_sname,
                    (unsigned long)((char*)pc - (char*)di.dli_saddr));
        else
            fprintf(stderr, "[SPIN]   %p\n", pc);
    }
    bo2_guest_stack_of_runner(why);
}

static void qos_note(const char* qui, int idx, bool demander_ui);   // P654/C9, plus bas
#if defined(__ANDROID__)
extern "C" void bo2_perf_register_thread(int role) __attribute__((weak));   // Bo2-APK adpf.cpp (ADPF)
#endif
// P995 : gfx_register_* jette tout tant que la fenetre n'est pas ouverte. L'invite Steam, sans les traces de
// un autre build, cree ses nuanceurs et ses cibles avant elle : monde noir, 27 pipelines. Il attend donc la fenetre.
static std::atomic<bool> g_fenetre_tranchee{false};
static void* win32_main_thread_runner(void* p) {
    while (bo2_env_on("BO2_WINDOW") && !g_fenetre_tranchee.load(std::memory_order_acquire)) usleep(1000);
    win32_install_crash_handler();     // per-thread sigaltstack for THIS host thread
    { struct sigaction sa{}; sa.sa_sigaction = spin_sigprof; sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGPROF, &sa, nullptr); }
    g_sched_thread = pthread_self();
    pthread_setname_np("bo2_ordonnanceur");   // P795 : [FILS-CPU]
    g_sched_thread_valid.store(true, std::memory_order_release);
    qos_note("ordonnanceur", 0, bo2_env_on("BO2_QOS"));
#if defined(__ANDROID__)
    if (bo2_perf_register_thread) bo2_perf_register_thread(0);   // ADPF : la boucle principale du jeu
#endif
    auto* a = (MainRunArgs*)p;
    win32_run_main_thread_impl(a->st, a->entry);
    g_guest_finished.store(true, std::memory_order_release);
    return nullptr;
}
extern "C" void win32_run_main_thread(State* st, uint32_t entry) {
    static const size_t MAIN_HSTACK = 1024ull * 1024 * 1024;   // 1 GB (lazily committed)
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, MAIN_HSTACK);
    MainRunArgs args{st, entry};
    pthread_t th;
    if (pthread_create(&th, &attr, win32_main_thread_runner, &args) != 0) {
        fprintf(stderr, "[main-thread] big-stack pthread_create failed — running on default stack\n");
        pthread_attr_destroy(&attr);
        win32_main_thread_runner(&args);
        return;
    }
    pthread_attr_destroy(&attr);

    // BO2_WINDOW=1: the guest owns the worker thread, so the REAL process main
    // thread is free — and AppKit only works there. Instead of blocking in join,
    // drive the Metal window from here until the guest finishes (Gate B step 1).
    // Default (unset) keeps the exact verified headless behaviour: a plain join.
    if (bo2_env_on("BO2_WINDOW")) {
        extern bool gfx_window_open(int, int);
        extern bool gfx_pump_once();
        extern void gfx_shutdown();
        const bool fenetre = gfx_window_open(1280, 720);
        g_fenetre_tranchee.store(true, std::memory_order_release);
        if (fenetre) {
            // Poll the guest rather than joining, so the UI stays live.
            // 🎯 BO2_PUMP_US — THE ONE REAL-TIME THREAD THE GUEST TALKS TO.
            //
            // Everything else in this port is cooperative: guest threads are
            // ucontext coroutines on a single pthread, and no host thread ever
            // writes guest memory. This loop is the exception -- it runs on the
            // process main thread, on a wall-clock sleep, and the guest
            // synchronises with what it does. So it is the only quantity left
            // that a FASTER GUEST changes: fewer pump ticks per unit of guest
            // work. That fits every result we have -- BO2_W32_BURN repairs the
            // fast barrier because it slows the guest in REAL time, and
            // BO2_CLOCK_SCALE does not because it slows nothing real.
            //
            // Making it a knob tests that in both directions on one build: a
            // shorter sleep with the fast barrier should repair it, and a
            // longer sleep with the default barrier should break it. Default
            // 4000 is the verified behaviour, unchanged.
            static const unsigned pump_us = [] {
                const char* e = bo2_getenv("BO2_PUMP_US");
                const int v = e ? atoi(e) : 4000;
                if (v != 4000)
                    fprintf(stderr, "[PUMP] %d us between gfx pumps (default 4000)\n", v);
                return (unsigned)(v > 0 ? v : 1);
            }();
            if (const char* e = bo2_getenv("BO2_ACOUP_MS"); e && atoi(e) > 0) {   // [A-COUP] : le veilleur, plus haut
#if defined(__APPLE__)
                g_pompe_port = pthread_mach_thread_np(pthread_self());
#endif
                pthread_t v;
                if (pthread_create(&v, nullptr, acoup_veilleur, (void*)(intptr_t)atoi(e)) == 0) pthread_detach(v);
            }
            while (!g_guest_finished.load(std::memory_order_acquire)) {
                g_pompe_passes.fetch_add(1, std::memory_order_relaxed);
                if (!gfx_pump_once()) break;      // window closed by the user
                usleep(pump_us);                  // ~240 Hz by default
            }
            // The guest stops at HARDCAP long before a human can look at the
            // result, so keep the last presented frame on screen until the
            // window is closed. BO2_WINDOW_HOLD=0 opts out for unattended runs.
            const char* hold = bo2_getenv("BO2_WINDOW_HOLD");
            if (!hold || atoi(hold) != 0) {
                fprintf(stderr, "[gfx] guest finished — window held open, close it to exit\n");
                while (gfx_pump_once()) usleep(8000);
            }
            gfx_shutdown();
        }
    }
    pthread_join(th, nullptr);
}

// ── 🎯 BO2_HOST_THREADS — SORTIR UNE COROUTINE SUR UN VRAI FIL HOTE ─────────
//
// LE DEFAUT QUE CA CORRIGE, MESURE (runs/sched1) :
// les douze coroutines invitees se partagent UN fil hote, et le jeu lui-meme
// (Main + Server + Backend) n'obtient que 43,8 % de ce cœur. 31,9 % vont aux
// quatre ouvriers du pool de jobs et 23,2 % au son — c'est-a-dire a des threads
// que Windows fait tourner sur d'AUTRES cœurs. Sortir le son rend 1,30 % au
// jeu, les ouvriers 1,47x, les deux 2,28x.
//
// Valeurs : « snd » (Sound Mix), « snddec » (Sound Decode), « jq » (les quatre
// ouvriers), « webm », « srv » (Server, param 0xa), « backend » (Backend, param
// 0x1). Plusieurs, separees par des virgules.
//
// P647 (2026-09-25) : en JSS le fil ordonnanceur se partage par tours entre Main
// (39 %), le rendu Backend (25 %) et le serveur (27 %) ; le serveur n'a qu'un
// tiers de cœur, d'ou le monde a 0,30-0,36x. Sous Windows chacun a son cœur.
//
// ⚠️ DEFAUT VIDE = COMPORTEMENT D'AUJOURD'HUI, BIT POUR BIT. Rien n'est heberge,
// `g_any_hosted` reste faux, et le proxy `g_cur` ne touche meme pas au TLS.
#if defined(BO2_TARGET_SPS)
// Campagne Steam : CreateThread(0x6e95b0, indice 1/0xa..0xf) et les ouvriers « JQ Worker %d » (0x8b5b50, P958).
static constexpr uint32_t kThreadMain = 0x006e95b0u, kJqWorker = 0x008b5b50u;
#elif defined(BO2_TARGET_MPS)
// multi / Zombies Steam : images d'une ancienne version de t6mp, 0x890e30 (motif masque unique) et 0xa008c0 (table de correspondance).
static constexpr uint32_t kThreadMain = 0x006db650u, kJqWorker = 0x009b4aa0u;
#else
static constexpr uint32_t kThreadMain = 0x006dc620u, kJqWorker = 0x009b1e10u;
#endif
static bool host_wanted(uint32_t proc, uint32_t param) {
    static const char* spec = bo2_getenv("BO2_HOST_THREADS");
    if (!spec || !*spec) return false;
    auto has = [](const char* sp, const char* key) {
        const size_t n = strlen(key);
        for (const char* p = sp; (p = strstr(p, key)); p += n) {
            const bool lo = (p == sp) || p[-1] == ',';
            const bool hi = (p[n] == 0) || p[n] == ',';
            if (lo && hi) return true;
        }
        return false;
    };
    if (proc == kJqWorker)                    return has(spec, "jq");
    if (proc == kThreadMain && param == 0x0du) return has(spec, "snd");
    if (proc == kThreadMain && param == 0x0eu) return has(spec, "snddec");
    if (proc == kThreadMain && param == 0x0fu) return has(spec, "webm");
    if (proc == kThreadMain && param == 0x0au) return has(spec, "srv");
    if (proc == kThreadMain && param == 0x01u) return has(spec, "backend");
    if (proc == kThreadMain && param == 0x0bu) return has(spec, "db");       // Database (Z-NATIF-15f, mesure)
    if (proc == kThreadMain && param == 0x0cu) return has(spec, "stream");   // Stream : lectures disque (15f-4, mesure)
    return false;
}

// Point d'entree d'un fil hote. `t_host_tid` est pose AVANT toute execution
// invitee : `thread_trampoline` commence par `g_threads[g_cur]`, et le proxy
// `g_cur` ne rendra le bon indice que si ce fil s'est deja nomme.
// ── P654/C9 — LA QoS DES FILS HOTES ─────────────────────────────────────────
// Ce Mac a 4 coeurs P et 4 coeurs E, et aucun fil hote ne declarait de QoS :
// sous P651b, ~7 fils occupes pour 4 coeurs P, donc 3 sur des coeurs E, et rien
// n'empechait que ce soit le fil ordonnanceur (Main, le goulot). Sous Windows, sur
// un PC a coeurs homogenes, la question ne se pose pas.
// BO2_QOS=1 met le fil ordonnanceur en USER_INTERACTIVE. Le temoin [QOS] relit la
// classe obtenue dans les DEUX bras, pour l'ordonnanceur et chaque fil heberge.
static void qos_note(const char* qui, int idx, bool demander_ui) {
    if (demander_ui) {
        const int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        if (rc != 0) fprintf(stderr, "[QOS] %s %d : pthread_set_qos_class_self_np -> %d\n",
                             qui, idx, rc);
    }
    fprintf(stderr, "[QOS] %s %d : classe=0x%02x (%s)\n", qui, idx, (unsigned)qos_class_self(),
            demander_ui ? "BO2_QOS, BO2_QOS_SRV ou BO2_PRIO_QOS, demande USER_INTERACTIVE 0x21" : "par defaut");
}

static void* host_thread_entry(void* arg) {
    t_host_tid = (int)(intptr_t)arg;
    { char nm[32]; snprintf(nm, sizeof nm, "bo2_h%d_%06x", t_host_tid, g_threads[t_host_tid]->proc);
      pthread_setname_np(nm); }   // P795 : [FILS-CPU] et `sample` nomment le fil
    // P799 : BO2_QOS_SRV=1 met le fil Server (0x80dee0, param 0xa) en USER_INTERACTIVE. En combat (P797) 5,3 coeurs
    // sont demandes pour 4 coeurs P et le serveur, qui fait avancer le monde, partage les siens ; sous Windows il a un
    // coeur plein. Temoin : [QOS] fil heberge <tid> : classe=0x21.
    // P803 : BO2_PRIO_QOS=1 traduit HIGHEST/TIME_CRITICAL (SetThreadPriority >= 2, pose avant ResumeThread) en
    // USER_INTERACTIVE : sous Windows ces fils passent devant les autres ; ici c'est la classe qui donne un coeur P.
    // Les deux par defaut depuis P803 (starved 908-1872 -> 7-71, img/s 34,6 -> 34,0, monde 0,99x) ; =0 pour retirer.
    const GThread* g = g_threads[t_host_tid];
#if defined(__ANDROID__)
    // ADPF (Bo2-APK android/native/src/adpf.cpp) : les fils de l'image (backend D3D, serveur, ouvriers jq) rejoignent
    // la session d'indications de performance, qui donne au gouverneur l'echeance de l'image. Le son n'y est pas.
    {
        const int role = g->proc == kJqWorker ? 3 : (g->proc == kThreadMain && g->param == 0x01u) ? 1
                       : (g->proc == kThreadMain && g->param == 0x0au) ? 2 : -1;
        if (role >= 0 && bo2_perf_register_thread) bo2_perf_register_thread(role);
    }
#endif
    qos_note("fil heberge", t_host_tid,
             (g->proc == kThreadMain && g->param == 0x0au && bo2_env_on("BO2_QOS_SRV", true))
             || (g->prio >= 2 && bo2_env_on("BO2_PRIO_QOS", true)));
    thread_trampoline();
    return nullptr;
}

// ── 🔴 CE QUE LE PREMIER ESSAI A RATE, ET POURQUOI C'EST ECRIT ICI ───
// L'hebergement etait decide dans `spawn_thread` sous `if (!suspended && ...)`.
// Resultat : ZERO coroutine hebergee, en SILENCE. Cause, mesuree dans
// runs/snd0_selecteur_muet : les 18 CreateThread du moteur passent TOUS
// `flags=0x4` = CREATE_SUSPENDED, sans exception. Le jeu cree ses fils endormis,
// leur ecrit leur identifiant (l'out-parametre lpThreadId, dont le moteur se
// sert pour se reconnaitre), PUIS les reveille. Le seul instant ou un fil invite
// devient reellement vivant est donc `ResumeThread`.
// ⇒ La decision se prend ICI, et les DEUX chemins l'appellent.
//
// 🔴 P628/E0 — MAIS LA MOITIE DU CORRECTIF MANQUAIT, ET C'EST CE QUI A FAIT
// DECLARER L'HEBERGEMENT « INSTABLE » A P615. Deplacer la decision ici etait
// juste ; l'appeler SANS CONDITION depuis `spawn_thread` ne l'etait pas. Le
// commentaire ci-dessus dit lui-meme pourquoi : le moteur cree le fil endormi,
// lui ecrit son identifiant (l'out-parametre `lpThreadId` de `CreateThread`,
// ecrit APRES le retour de `spawn_thread`, voir impl_CreateThread plus bas),
// PUIS le reveille. Heberger des la creation demarre le code invite AVANT cette
// ecriture. Mesure 4/4 dans runs/p615_J_* : la ligne [HOSTTID] y sort AVANT la
// ligne [CreateThread] du meme fil.
// ⇒ a la creation, un fil SUSPENDU est laisse au repos ; son hebergement est
//   DIFFERE jusqu'a `ResumeThread`, le seul instant ou il devient vivant.
// ⚠️ NE PAS remplacer `at_spawn` par un simple `t->status == T_SUSPENDED` :
//    `impl_ResumeThread` appelle `maybe_host` AVANT de poser T_RUN, donc le
//    statut y vaut ENCORE T_SUSPENDED. Ce serait le retour exact du
//    zero-heberge-en-silence de runs/snd0_selecteur_muet.
static void maybe_host(int idx, bool at_spawn) {
    if (idx < 0 || idx >= (int)sched_nthreads()) return;
    GThread* t = g_threads[idx];
    if (t->hosted || t->status == T_DONE) return;
    // ⚠️ `host_wanted` D'ABORD : place apres, le temoin ci-dessous s'imprimerait
    // sur les 18 CreateThread de CHAQUE run, y compris sans hebergement arme —
    // donc changerait les journaux du regime par defaut.
    if (!host_wanted(t->proc, t->param)) return;
    if (at_spawn && t->status == T_SUSPENDED) {
        fprintf(stderr, "[HOSTTID] coroutine %d creee SUSPENDUE : hebergement "
                        "differe jusqu'a ResumeThread\n", idx);
        return;
    }
    t->hosted    = true;
    __atomic_store_n(&t->hsem, dispatch_semaphore_create(0), __ATOMIC_RELEASE);   // C25, avant le fil
    // P641 : l'appelant est un shim, donc du code leve ; s'il n'est pas heberge,
    // c'est le fil ordonnanceur. Pose AVANT g_any_hosted (meme fil : ordre du
    // programme ; les fils heberges n'existent pas encore au premier passage).
    if (t_host_tid < 0) bo2_sched_tp = bo2_read_tp();
    g_any_hosted = true;
    g_hosted_n++;
    // ⚠️ LES INSTRUCTIONS `lock` DE L'INVITE DOIVENT DEVENIR DE VRAIES
    // ATOMIQUES. Sans ca, deux fils hotes font des lectures-modifications-
    // ecritures qui se perdent, et le symptome sera un compteur de
    // references qui derive — c'est-a-dire le gel qu'on a passe des jours a
    // nommer. On le DIT plutot que de le supposer.
    if (const char* a = bo2_getenv("BO2_HW_ATOMICS"); !a || atoi(a) == 0)
        fprintf(stderr, "[HOSTTID] ⛔ coroutine %d hebergee avec BO2_HW_ATOMICS=0 : "
                        "les instructions verrouillees de l'invite ne sont que des "
                        "barrieres de COMPILATION. Arme BO2_HW_ATOMICS=2.\n", idx);
    fprintf(stderr, "[HOSTTID] coroutine %d (proc=0x%08x param=0x%x) SORT du "
                    "tourniquet et prend son propre fil hote\n", idx, t->proc, t->param);
    // ⚠️ PAS `std::thread` : sur macOS un fil secondaire recoit 512 Kio de
    // pile par defaut, alors que TOUTE coroutine de ce fichier en recoit
    // COSTACK = 8 Mio (ligne ~729). Le code invite lifte empile des cadres
    // hotes profonds ; heberger avec 16 fois moins de pile ferait deborder
    // silencieusement, et le defaut se lirait « le multicœur casse le jeu »
    // alors qu'il ne dirait que « on a change la pile sans le vouloir ».
    // La comparaison n'a de sens qu'a pile EGALE.
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, COSTACK);
    if (pthread_create(&t->hthread, &at, host_thread_entry,
                       (void*)(intptr_t)idx) != 0) {
        fprintf(stderr, "[HOSTTID] ⛔ pthread_create a echoue pour la coroutine %d — "
                        "elle RESTE dans le tourniquet\n", idx);
        t->hosted = false; g_hosted_n--;
        if (g_hosted_n == 0) g_any_hosted = false;
    }
    pthread_attr_destroy(&at);
}

static uint32_t spawn_thread(uint32_t proc, uint32_t param, bool suspended,
                             uint32_t req_stack = 0) {
    sched_init();
    const int status0 = suspended ? T_SUSPENDED : T_RUN;
    GThread* t = nullptr;
    int idx = -1;
    // 🎯 Z-NATIF-12e — UN FIL TERMINE REND SA CASE. Demonware cree un fil par recherche de nom (bdThread,
    // proc 0xadaa30) : ~2,5 par seconde tant que le STUN ne repond pas. Rien n'etait jamais repris : 987 cases,
    // 1,27 Go de piles invitees et ~9 Go d'empreinte en 397 s, puis SIGSEGV dans SetEvent
    // (run du 08/10). Windows libere la pile a la fin du fil et l'objet a la fermeture de sa
    // derniere poignee ; ici la case d'un fil T_DONE dont le jeu a ferme la poignee sert au fil suivant, avec sa
    // pile hote et ses blocs invites (thread_trampoline les remet a zero).
    // Seulement depuis le fil ordonnanceur : une coroutine T_DONE qui n'y tourne pas a deja fait son dernier
    // coop_switch, plus rien n'ecrira donc son sw_jb. Un fil HOTE qui cree un fil ne peut pas le savoir : il
    // ajoute une case, comme avant. Jamais la case d'un fil heberge (son pthread peut etre encore en sortie).
    // ponytail: campagne laissee telle quelle (24 fils au plus, mesure) ; lever le #if si elle en cree sans fin.
    if (!k32_hors_ordonnanceur()) {
        std::lock_guard<std::mutex> lk(g_spawn_mx);
        for (size_t i = 1, n = sched_nthreads(); i < n && idx < 0; i++) {
            const GThread* d = g_threads[i];
            if (d->status == T_DONE && d->handle_closed && !d->hosted) idx = (int)i;
        }
    }
    if (idx >= 0) {
        t = g_threads[idx];                           // status reste T_DONE tant que tout n'est pas pret
        t->sw_live = false; t->cur_state = nullptr; t->handle_closed = false; t->unwind_env = 0;
        t->gsc_saved = false; t->sleep_until_ns = 0; t->wfso_until_ns = 0; t->prio = 0;
        if (idx < 64) g_cs_waiting_for[idx] = 0;      // 14z : une attente du fil d'avant ne compte pas pour celui-ci
        std::memset(t->fls, 0, sizeof t->fls);
        std::memset(t->gsc_scratch, 0, sizeof t->gsc_scratch);
        static uint64_t repris;
        if (++repris, (repris & (repris - 1)) == 0)   // 1er, 2e, 4e, 8e…
            fprintf(stderr, "[FIL-RECYCLE] n°%llu : la case %d (fil termine proc=0x%08x) sert a proc=0x%08x ; %zu cases\n",
                    (unsigned long long)repris, idx, t->proc, proc, sched_nthreads());
    }
    if (!t) { t = new GThread(); t->hstack = (char*)malloc(COSTACK); }
    t->req_stack = req_stack;
    t->proc = proc; t->param = param; t->wait_h = 0;
    t->wake_token = false; t->unwinding = false; t->wake_at = 0; t->resume_pc = 0; t->dispatch_armed = false;
    getcontext(&t->ctx);
    t->ctx.uc_stack.ss_sp = t->hstack; t->ctx.uc_stack.ss_size = COSTACK; t->ctx.uc_link = nullptr;
    makecontext(&t->ctx, thread_trampoline, 0);
    // ⚠️ LA REALLOCATION EST LA COURSE QU'ON NE VERRAIT PAS. `sched_init` reserve
    // 64 entrees ; un fil hote lit `g_threads[idx]` sans verrou, ce qui est SUR
    // tant que le tampon du vecteur ne bouge pas. Le hurler AVANT plutot que de
    // debugger une lecture de memoire liberee (max observe a ce jour : 24 fils).
    if (idx >= 0) {
        __atomic_store_n(&t->status, status0, __ATOMIC_RELEASE);   // case reprise : visible une fois prete
    } else {
        t->status = status0;
        std::lock_guard<std::mutex> lk(g_spawn_mx);
        if (g_any_hosted && g_threads.size() + 1 >= g_threads.capacity())
            fprintf(stderr, "[HOSTTID] \u26d4 %zu fils pour une capacite de %zu : le prochain "
                            "push_back REALLOUE g_threads pendant qu'un fil hote le lit. "
                            "Augmente le reserve() de sched_init.\n",
                    g_threads.size() + 1, g_threads.capacity());
        g_threads.push_back(t);
        idx = (int)g_threads.size() - 1;
        g_threads_pub.store(g_threads.size(), std::memory_order_release);  // APRES l'element
    }
    uint32_t h = ++t_fake_handle_counter;
    // P628/E3 : insertion = verrou EXCLUSIF. C'est elle qui peut rehacher.
    { std::unique_lock<std::shared_mutex> lk(g_ev_mx); g_thread_handle[h] = idx; }
    maybe_host(idx, /*at_spawn=*/true);
    return h;
}

static void impl_CreateThread(State& s) {
    uint32_t start = win32_arg(s, 2), param = win32_arg(s, 3), flags = win32_arg(s, 4);
    // 🎯 dwStackSize (arg 1) SE LIT ICI, AVANT win32_stdcall_ret, ET C'ETAIT LE BUG.
    //
    // Il etait lu APRES : `win32_stdcall_ret(s, 6)` puis `win32_arg(s, 1)`. Or
    // stdcall_ret fait `rsp += 4 + 6*4` = 0x1c, et win32_arg lit `rsp + 4 + i*4`.
    // Donc `win32_arg(s, 1)` apres le depilage lisait `ancien_esp + 0x24` —
    // QUATRE DWORDS AU-DELA du dernier argument, c'est-a-dire ce que l'appelant
    // avait laisse plus haut sur sa pile. Les cinq autres arguments etaient bien
    // lus avant ; seul celui-la avait glisse de l'autre cote du depilage.
    //
    // Mesure qui l'a prouve (runs/play-angola, crash SIGBUS du 2026-09-17) :
    // tid 42 a recu stk=0x80000000 esp=0x809befe0, et
    // esp = stk + stksz - 0x20 donne stksz = 0x9BF000, soit 10,2 Mo, pour un
    // thread dont le jeu ne demande pas 10 Mo. Chaque pile invitee etait donc
    // dimensionnee sur une valeur parasite, sauvee seulement par le plancher de
    // 1 Mio quand le parasite etait petit.
    //
    // Ce que ce correctif ne prouve PAS : que c'est la cause du SIGBUS de tid 42.
    // La carte invitee est PLATE sur 4 Gio (win32_runtime.cpp:106/121), donc
    // 0x80000000 est le debut LEGITIME du segment 2 et 0x809befe4 est bien
    // mappe — voir la retraction 102, qui retire « la pile a atterri au-dessus
    // de la region mappee ». Ce qui est etabli ici, c'est seulement que la
    // TAILLE venait d'une valeur parasite.
    const uint32_t dwStackSize = win32_arg(s, 1);
    // 🎯 lpThreadId (arg 5) WAS IGNORED, AND THAT IS WHY ANGOLA NEVER TICKS.
    //
    // The engine identifies its threads by ID, not by handle: five predicates
    // (0x80df30, 0x4deaf0, 0x528470, 0x5be020, 0x499ce0) each compare
    // GetCurrentThreadId() against one global, and those globals are filled by
    // passing their address as CreateThread's lpThreadId — e.g. 0x499ace does
    //     push 0x29255b0 ; push 4 ; push 0xa ; push 0x80dee0 ; ... ; call CreateThread
    //
    // Ignoring that out-parameter left four of the five globals at ZERO
    // (measured: 0x80df30=4096, the other four 0), so when the save asked
    // sub_4c5e70 "which thread am I?" from tid 12 (id 4108) NO predicate matched,
    // sub_529f90 raised Com_Error("SaveDevice_PC : cannot open file for saving"),
    // 0x4446fe set [0x287bb30]=1, and SV_Frame parked at 0x852b70 sleeping 50 ms
    // for ever without ever reaching its tick gate at 0x852bb9.
    //
    // The id must match what impl_GetCurrentThreadId reports for that thread —
    // 0x1000 + its scheduler index — or the comparison still fails.
    const uint32_t lpThreadId = win32_arg(s, 5);
    win32_stdcall_ret(s, 6);
    uint32_t h = spawn_thread(start, param, (flags & 0x4u) != 0, dwStackSize);
    uint32_t gtid = 0;
    {
        const int idx = th_find(h);                   // P628/E3
        if (idx >= 0) {
            gtid = 0x1000u + (uint32_t)idx;
            if (lpThreadId) win32_mem_write32(lpThreadId, gtid);
        }
    }
    fprintf(stderr, "[CreateThread] proc=0x%08x param=0x%08x flags=0x%x -> handle 0x%x "
            "id=%u lpThreadId=0x%08x%s\n", start, param, flags, h, gtid, lpThreadId,
            lpThreadId ? "  (id written back — the engine identifies threads by ID)" : "");
    s.gpr.rax.dword = h;
}
static void impl_ResumeThread(State& s) {
    uint32_t h = win32_arg(s, 0);
    win32_stdcall_ret(s, 1);
    const int idx = th_find(h);                       // P628/E3
    if (idx >= 0 && g_threads[idx]->status == T_SUSPENDED) {
        // ⚠️ HEBERGER AVANT DE POSER T_RUN : le tourniquet n'elit que des T_RUN
        // non heberges. Dans l'autre ordre la coroutine serait elligible une
        // fraction de tour — et deux fils dans le meme contexte, ce n'est pas une
        // course rare, c'est une corruption immediate.
        maybe_host(idx, /*at_spawn=*/false);
        g_threads[idx]->status = T_RUN;
    }
    s.gpr.rax.dword = 1;                              // previous suspend count (>0)
}

// --- events (cooperative) ---
static void impl_CreateEventA(State& s) {
    uint32_t manual = win32_arg(s, 1), initial = win32_arg(s, 2);
    uint32_t retaddr = win32_mem_read32(s.gpr.rsp.dword);
    win32_stdcall_ret(s, 4);
    uint32_t h = ++t_fake_handle_counter;
    // P628/E3 : insertion = verrou EXCLUSIF. C'est elle qui peut rehacher.
    { std::unique_lock<std::shared_mutex> lk(g_ev_mx);
      g_events[h] = { manual != 0, initial != 0, false, 0u }; }
    fprintf(stderr, "[CreateEvent] h=0x%x manual=%u initial=%u ret=0x%08x tid=%d\n",
            h, manual, initial, retaddr, (int)g_cur);
    s.gpr.rax.dword = h;
}
// Capped per-(callsite,handle) logger for the sync-primitive flow.
// The fastfile async-ring callsites (slot-event Reset/Set/WFSO) get a much
// higher cap: the patch.ff steady state cycles them ~30x and the desync
// forensics need the full ordering, not just the first 6.
// 🎯 BO2_EVCT=1 — UNCAPPED per-handle event tallies.
//
// ⚠️ WHY THIS EXISTS. sync_log below is capped at 6 per (callsite, handle), so
// COUNTING ITS LINES COUNTS THE CAP. On 2026-09-04 that produced "event 0x10012:
// 6 waits, 5 SetEvents — the loader is one signal short", which was reported as
// the root cause and is nothing of the kind: the 6 is the cap. Same family as
// every capped-probe retraction this project made.
//
// These counters are flat arrays indexed by the low byte of the handle (fake
// handles are minted sequentially from a small base), no allocation, no hashing,
// so they are safe to leave on during a REPLAY — BO2_PROFILE's per-import
// unordered_map insert makes a replay diverge, and this must not.
enum { kEvSlots = 256 };
uint32_t g_ev_set[kEvSlots], g_ev_wait[kEvSlots], g_ev_woke[kEvSlots],
         g_ev_reset[kEvSlots], g_ev_pulse[kEvSlots], g_ev_block[kEvSlots];
static inline int ev_slot(uint32_t h) { return (int)(h & 0xffu); }
extern "C" void bo2_event_report(const char* why) {
    if (!bo2_getenv("BO2_EVCT")) {
        fprintf(stderr, "[EVCT] not armed (set BO2_EVCT=1) — this run says NOTHING "
                        "about how often any event was signalled\n");
        return;
    }
    fprintf(stderr, "\n[EVCT] %s — UNCAPPED per-handle tallies\n", why ? why : "report");
    fprintf(stderr, "[EVCT] handle      SetEvent   woke   Pulse   Reset     WFSO  blocked\n");
    for (int i = 0; i < kEvSlots; i++) {
        if (!(g_ev_set[i] | g_ev_wait[i] | g_ev_pulse[i] | g_ev_reset[i])) continue;
        fprintf(stderr, "[EVCT] 0x%05x  %9u %6u %7u %7u %8u %8u%s\n",
                0x10000 + i, g_ev_set[i], g_ev_woke[i], g_ev_pulse[i],
                g_ev_reset[i], g_ev_wait[i], g_ev_block[i],
                (g_ev_wait[i] > g_ev_set[i] + g_ev_pulse[i]) ? "   <<< more waits than signals" : "");
    }
}

static bool sync_log(uint32_t retaddr, uint32_t h, int cap = 6) {
    static DiagCounter<14> s_counts;   // P660/C14 : appelée hors verrou par tous les fils hébergés
    if (retaddr == 0x4dc34du || retaddr == 0x48d2adu || retaddr == 0x5d036fu)
        cap = 400;
    return s_counts.bump(((uint64_t)retaddr << 32) | h) < (uint32_t)cap;
}

static void hs_evt(const char* op, uint32_t h) {
    if (h == 0x1001eu || h == 0x10020u || h == 0x10021u) win32_handshake_note(op, h);
}

// Ordered, UNCAPPED history of the operations on the fastfile read ring's four
// slot events (0x10019..0x1001c): sync_log caps at 6 lines per site, which lost
// the order that decides a lost wakeup (a SetEvent that lands BEFORE the waiter's
// ResetEvent is erased by it). S=set R=reset W=wait-enter w=wait-return.
struct EvOp { char op; int8_t tid; uint8_t sig; uint32_t h, ret; uint64_t seq; };
static EvOp g_evop[48]; static uint64_t g_evop_n;
static inline void evop_note(char op, uint32_t h, uint32_t ret, int sig) {
    if (h < 0x10019u || h > 0x1001cu) return;
    EvOp& o = g_evop[g_evop_n % 48u]; o.op = op; o.tid = (int8_t)g_cur; o.sig = (uint8_t)sig; o.h = h; o.ret = ret; o.seq = ++g_evop_n;
}
static void evop_report() {
    const uint64_t n = g_evop_n, from = n > 48 ? n - 48 : 0;
    fprintf(stderr, "[RING-EV] last %llu operations on the read-ring slot events (op tid handle caller signalled-after):\n",
            (unsigned long long)(n - from));
    for (uint64_t k = from; k < n; k++) { const EvOp& o = g_evop[k % 48u];
        fprintf(stderr, "[RING-EV]   #%llu %c tid %d h=0x%x from 0x%08x sig=%u\n", (unsigned long long)o.seq, o.op, o.tid, o.h, o.ret, o.sig); }
}
static void impl_SetEvent(State& s) {
    uint32_t h = win32_arg(s, 0);
    uint32_t retaddr = win32_mem_read32(s.gpr.rsp.dword);
    evop_note('S', h, retaddr, 1);
    // ⚠️ ONE FRAME FURTHER OUT, AND IT IS ONLY VALID FOR A ONE-PUSH FORWARDER.
    // The render work event is signalled from 0x544080, which is four
    // instructions — `mov eax,[0x29253bc]; push eax; call SetEvent; ret` — so at
    // this point [esp+8] is that forwarder's OWN caller, which is the frame worth
    // naming. For any other shape of caller it is that frame's data, NOT a return
    // address, which is exactly the mistake [IMP-SITE] made tonight by applying
    // one wrapper's layout to everybody. Printed as `via` and never ranked on.
    uint32_t via = win32_mem_read32(s.gpr.rsp.dword + 8u);
    win32_stdcall_ret(s, 1);
    sync_note("SetEvent", h, retaddr, g_cur);
    hs_evt("SetEvent", h);
    g_ev_set[ev_slot(h)]++;
    GEvent* ev = ev_find(h);                          // P628/E3
    int woken = 0;
    if (ev) {
        // 🎯 P648/C1 — DEKKER. Cote signaleur : publier `signaled`, BARRIERE, puis
        // lire les attendeurs. Cote attendeur (WFSO, plus bas) : publier T_BLOCKED,
        // BARRIERE, puis relire `signaled`. Sans les deux barrieres, ARM (et meme
        // x86) laisse chaque lecture passer devant l'ecriture de son propre fil :
        // le serveur heberge lit Main « pas encore bloque » pendant que Main lit
        // l'evenement « pas encore signale », et Main dort pour toujours sur un
        // evenement signale. Mesure P647 (031837) : `[EVENTS] 0x1003b=Sm`, tid 0
        // bloque INFINI dessus en 0x47c57e. Windows n'a pas ce trou : son
        // dispatcher prend un verrou autour des deux gestes.
        __atomic_store_n(&ev->signaled, true, __ATOMIC_RELAXED);
        if (g_cur >= 0 && g_cur < 32) ev->signalers |= (1u << g_cur);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        // P651/C6 : la taille PUBLIEE, pas la fin du vecteur. `for (auto* t : g_threads)` lisait une case que
        // push_back n'avait pas encore remplie vue de ce cœur : SIGSEGV fault_addr=0x74756f202020244a d'un fil
        // heberge pendant un CreateThread (run du 08/10, Z-NATIF-12d).
        for (size_t i = 0, n = sched_nthreads(); i < n; i++) {
            GThread* t = g_threads[i];
            if (__atomic_load_n(&t->status, __ATOMIC_RELAXED) == T_BLOCKED &&
                __atomic_load_n(&t->wait_h, __ATOMIC_RELAXED) == h) {
                t->wake_at = 0;
                __atomic_store_n(&t->status, (int)T_RUN, __ATOMIC_RELAXED);
                woken++;
                if (t->hosted) host_wake(t);                   // C25
                else dispatch_semaphore_signal(ordo_sem());    // P796
            }
        }
    }
    if (sync_log(retaddr, h))
        fprintf(stderr, "[SetEvent] h=0x%x ret=0x%08x via=0x%08x known=%d woke=%d tid=%d\n",
                h, retaddr, via, ev != nullptr, woken, (int)g_cur);
    s.gpr.rax.dword = 1;
    // Signalling is the one moment where another thread becomes able to make
    // progress, so it must be a scheduling point. Consumers that POLL (the render
    // backend checks its work event with WFSO(h, 0) once per loop) are not on the
    // blocked list, so waking blocked waiters is not enough — without a yield the
    // signaller runs on and can move the world out from under them. That is how
    // the frame handshake deadlocked: main queued a command list and signalled
    // 0x10020, then took the waiter depth to 1 and blocked in R_SyncRenderThread
    // before the backend ever looked at its work event; the backend then parked in
    // the frame-boundary spin waiting for a depth main could no longer release.
    // NOT inside a shim-driven guest callback, though — see g_in_shim_callback.
    if (!in_shim_cb() && (woken || (ev && ev->polled)))
        coop_yield_from("SetEvent");
}
static void impl_PulseEvent(State& s) {
    // Windows semantics: release the threads waiting RIGHT NOW (all for a
    // manual-reset event, one for auto-reset), leave the event unsignaled.
    // Delivery is a per-thread wake token consumed by the WFSO loop — merely
    // setting T_RUN would lose the pulse, since the waiter re-checks
    // `signaled` (still false) and re-blocks. Threads not yet at their wait
    // miss the pulse, exactly like on real Windows; the render-pool workers
    // are 1 ms pollers (WFSO timeout=1), so no lost-wakeup hazard.
    uint32_t h = win32_arg(s, 0);
    uint32_t retaddr = win32_mem_read32(s.gpr.rsp.dword);
    win32_stdcall_ret(s, 1);
    GEvent* ev = ev_find(h);                          // P628/E3
    int woken = 0;
    if (ev) {
        // 🎯 BO2_PULSE_INWAIT — UN PULSE N'ATTEINT AUCUN ATTENDEUR A DELAI FINI,
        // ET C'ETAIT MESURABLE DEPUIS TOUJOURS.
        // [EVCT] arme pour la premiere fois du depot (runs/w32_b) : handle
        // 0x10028, 209 575 Pulse, woke=0 sur CHACUN d'eux, pour 189 510 blocages
        // enregistres sur ce meme handle. Ce n'est pas une malchance, c'est une
        // impossibilite : l'attente a delai fini pose wake_at = g_tick + 1 puis
        // cede (l. ~2931/2949), et coop_yield commence par g_tick++ suivi de sa
        // passe de reveil (l. ~843-847) -- le thread repasse donc T_RUN dans le
        // MEME coop_yield qui vient de le deprogrammer, avant qu'une seule autre
        // coroutine ait tourne. Le test `status == T_BLOCKED` ci-dessous ne peut
        // jamais matcher un WFSO(h, 1). Sous Windows ce thread est REELLEMENT
        // parque 1 ms dans le noyau, et un PulseEvent dans cette fenetre le
        // libere : difference de fidelite avec un mecanisme exact, pas une garde.
        //
        // Ce qui dit vraiment « suspendu a l'interieur d'une attente sur h »,
        // c'est wait_h : il n'est pose qu'en entrant dans l'attente et seul
        // l'attendeur le remet a 0 en reprenant (l. ~2950), alors que `status`
        // et `wake_at`, eux, sont ecrases par la passe de reveil.
        //
        // Rayon d'action MESURE, pas suppose : sur tout un run, un seul handle a
        // Pulse > 0 -- 0x10028, le pool d'ouvriers de rendu. Opt-in tant qu'un
        // A/B n'a pas parle, meme discipline que BO2_PULSE_SEQ et BO2_WFSO_REALMS.
        static const bool inwait = bo2_env_on("BO2_PULSE_INWAIT");
        GThread* self = (g_cur >= 0 && g_cur < (int)sched_nthreads())
                      ? g_threads[g_cur] : nullptr;
        for (size_t i = 0, n = sched_nthreads(); i < n; i++) {   // P651/C6 : jamais la fin du vecteur
            GThread* t = g_threads[i];
            const bool hit = inwait ? (t != self && t->wait_h == h)
                                    : (t->status == T_BLOCKED && t->wait_h == h);
            if (hit) {
                t->status = T_RUN; t->wake_at = 0; t->wake_token = true; woken++;
                if (t->hosted) host_wake(t);                   // C25
                else dispatch_semaphore_signal(ordo_sem());    // P796
                if (!ev->manual) break;                        // auto-reset: wake one
            }
        }
        ev->signaled = false;
        ev->pulse_seq++;          // observable by a waiter already inside its wait
    }
    g_ev_pulse[ev_slot(h)]++; g_ev_woke[ev_slot(h)] += (uint32_t)woken;
    if (sync_log(retaddr, h))
        fprintf(stderr, "[PulseEvent] h=0x%x ret=0x%08x known=%d woke=%d tid=%d\n",
                h, retaddr, ev != nullptr, woken, (int)g_cur);
    s.gpr.rax.dword = 1;
}
static void impl_ResetEvent(State& s) {
    { uint32_t hh = win32_arg(s, 0); g_ev_reset[ev_slot(hh)]++; }
    uint32_t h = win32_arg(s, 0);
    uint32_t retaddr = win32_mem_read32(s.gpr.rsp.dword);
    win32_stdcall_ret(s, 1);
    sync_note("ResetEvent", h, retaddr, g_cur);
    hs_evt("ResetEvent", h);
    GEvent* ev = ev_find(h);                          // P628/E3
    if (ev) ev->signaled = false;
    evop_note('R', h, retaddr, 0);
    if (sync_log(retaddr, h))
        fprintf(stderr, "[ResetEvent] h=0x%x ret=0x%08x known=%d tid=%d\n",
                h, retaddr, ev != nullptr, (int)g_cur);
    s.gpr.rax.dword = 1;
}
// ── 🎯 BO2_REAL_MUTEX=1 — Win32 MUTEXES THAT EXCLUDE ─────────────────────────
// CreateMutexA was STUB_HANDLE (a fake handle), ReleaseMutex a STUB returning 0 —
// FAILURE in Win32 — and WaitForSingleObject treats any unknown handle as
// signalled, so acquiring one of the game's mutexes always succeeded and releasing
// it always "failed". The game creates 4 and releases ~150 per measured window
// (runs/cs_1, rec1). A mutex has an owning guest thread and a recursion count;
// another thread's wait yields until it is free; a dead owner makes it
// WAIT_ABANDONED (0x80) for the next taker, as on Windows.
struct GMutex { int owner = -1; uint32_t count = 0; };
static std::unordered_map<uint32_t, GMutex> g_mutexes;
static std::unordered_map<std::string, uint32_t> g_mutex_names;
// P661/C15 : les deux tables ET les champs GMutex. Deux fils hotes faisaient le
// test-et-prise de WFSO en meme temps et prenaient tous deux le meme mutex.
// Jamais tenu pendant une cession.
static std::mutex g_mutex_mx;
static uint64_t g_mutex_waits, g_mutex_abandoned;
static bool real_mutex_on() {
    static const bool on = [] { const char* e = bo2_getenv("BO2_REAL_MUTEX"); return e && atoi(e) != 0; }();
    return on;
}
extern "C" uint64_t bo2_mono_ns(void);   // defined below; `extern "C"` is a
                                         // linkage-spec, illegal at block scope.
// 🎯 UN SONDAGE QUI NE CEDE JAMAIS ARRETE LE JEU. Defaut ON.
//
// SV_Frame (_sub_852ad0=_bo2_sv_frame) cadence ses images ainsi, en 0x852b8a :
//     add edi,[ebp-0xc]   ; echeance += 1000/Hz  (division ENTIERE)
//     call 0x61f1b0       ; Sys_Milliseconds
//     cmp eax,edi ; jge 0x852bef ; push 0 ; call 0x57ee90   <- WFSO(h, 0)
// et la jointure en 0x852bfb est INCONDITIONNELLE : sur la branche « en retard »
// le resultat du sondage n'est meme pas lu, la boucle refait une image serveur
// et retombe sur le meme sondage. Or WFSO(h, 0) ne franchissait aucun point de
// cede, ni quand l'evenement est signale (on tombe avant la boucle `while`) ni
// quand il ne l'est pas (retour WAIT_TIMEOUT dans la boucle). L'ordonnanceur ne
// preempte jamais : la coroutine gardait donc l'unique thread hote pour toujours
// et affamait toutes les autres.
//
// Mesure : runs/fast2, 44 277 014 images serveur APRES le spawn (spawn ligne
// 110 263, LC #10000 ligne 121 401, LC #1000000 ligne 1 111 491). Le jeu avait
// l'air gele ; il tournait a fond dans cette boucle-ci.
//
// DIFFERENCE AVEC WINDOWS, pas optimisation : la-bas WFSO(h, 0) rend la main
// tout de suite aussi, mais le thread reste preemptible par le noyau et les
// autres tournent sur d'autres coeurs. Ici il n'y a qu'un thread hote.
//
// PORTEE VOLONTAIREMENT MINIMALE : seulement timeout==0. Une attente finie
// SATISFAITE ne cede toujours pas — y ceder couterait une rotation entiere au
// rappel OnBufferEnd et a deja plafonne le mixeur a un tampon par rotation.
//
// 🔴 REPASSE EN OPT-IN LE 2026-09-13, APRES MESURE (P66, n=3 + temoin meme
// binaire). Le correctif TIRE bel et bien — 65 536+ cedes par run, depuis au
// moins 5 sites — mais il n'achete RIEN de mesurable :
//     wy1..3 (ON)   maxDraws 4665/4741/4770   images 6803/7033/6999
//     wyoff1 (OFF)  maxDraws 4766             images 7094
//     nfp1   (OFF)  maxDraws 4690             images 7263
// et les bras SANS le correctif sont plutot au-dessus. Surtout, le temoin ne
// s'est pas emballe non plus : l'experience n'a donc pas reproduit le phenomene
// qu'elle devait trancher (runs/fast2 reste le SEUL emballement observe, n=1).
//
// On garde le code et l'analyse — le defaut de fidelite est reel, WFSO(h, 0) ne
// cede sur aucune de ses deux sorties immediates — mais on ne l'arme pas par
// defaut sur une hypothese non reproduite. C'est exactement ce contre quoi
// BO2_WFSO_REALMS met en garde vingt lignes plus haut : « OFF by default until
// an A/B says otherwise ». L'A/B a parle, et il a dit « rien ».
//
// Pour le rearmer quand on saura REPRODUIRE l'emballement : BO2_WFSO_POLL_YIELD=1.
static inline bool wfso_poll_yield_on() {
    static const bool on = [] {
        const char* e = bo2_getenv("BO2_WFSO_POLL_YIELD"); return e && atoi(e) != 0;
    }();
    return on;
}
static void impl_WaitForSingleObject(State& s) {
    uint32_t h = win32_arg(s, 0), timeout = win32_arg(s, 1);
    uint32_t retaddr = win32_mem_read32(s.gpr.rsp.dword);
    win32_stdcall_ret(s, 2);  // stdcall 2 args (handle, timeout) — a 3 here cost a
                              // 4-byte/call stack leak that poisoned EBP in
                              // callee-clean-reliant callers (FUN_0093e140).
    sched_init();
    sync_note("WFSO", h, retaddr, g_cur);
    // 🎯 REMEMBER WHO WAITS ON WHAT, so a DEADLOCK can be read from the host.
    // With BO2_TEARDOWN_REPAIR draining the shutdown registry, angola stops
    // spinning and instead stops DEAD: [CADENCE] imports(+0) svWorker(+0)
    // gscRun(+0) guestFrames(+0). Every counter frozen means every guest thread
    // is blocked, and nothing in the port could say on WHICH handle. Record it
    // here — the last wait each thread entered, its handle, its call site and a
    // sequence number — and let the host-clocked watch print it. The guest being
    // stuck is precisely when this must still be readable, which is why it is a
    // plain array written before the yield rather than anything the scheduler
    // owns.
    {
        extern void bo2_note_wait(int tid, uint32_t handle, uint32_t ret, uint32_t timeout);
        bo2_note_wait(g_cur, h, retaddr, timeout);
    }
    // 🎯 BO2_WFSO_REALMS=1 — A FINITE TIMEOUT MUST EXPIRE ON REAL MILLISECONDS.
    //
    // The default below counts `spins`, i.e. coop_yield() ROTATIONS, on the
    // stated assumption that one rotation is about a millisecond. WFSO(h, 100)
    // therefore waits 100 turns where Windows waits 100 ms, and that is a
    // fidelity difference with teeth: the work being awaited is NOT all guest
    // side. [DBWAIT] tid=0 waits while tid=12 signals [LOAD-COMPLETE], and that
    // load crosses real file I/O and decompression, whose progress is measured
    // in real time. Speed the guest up (BO2_W32_FAST) and the waiter burns its
    // budget of turns in less real time than before, so it times out where it
    // used to succeed — which is exactly the shape of the failure that only a
    // per-write COST (BO2_W32_BURN) has been able to hold off.
    //
    // Off by default until an A/B says otherwise: this changes scheduling, and
    // this repo has already retracted conclusions drawn from a knob switched on
    // because it sounded right.
    // 🎯 ET POUR UN FIL HEBERGE, CE N'EST PLUS UN REGLAGE : C'EST LA SEULE
    // SEMANTIQUE POSSIBLE. Le defaut compte le delai en TOURS D'ORDONNANCEUR
    // (`spins >= timeout`), sur l'idee qu'un tour vaut a peu pres une
    // milliseconde. Un fil sorti du tourniquet n'a plus de tour : son « tour »
    // est un `yield` de ~2,4 us, donc `WFSO(h, 1 ms)` rend la main 400 fois trop
    // tot et la boucle de sondage de l'ouvrier JQ devient un spin plein coeur
    // (P602 : 237 M appels dans une fenetre). ⇒ des qu'on est heberge, le delai
    // fini expire sur `bo2_mono_ns()`, aux TROIS sites (evenement, mutex, fil).
    //
    // 🔴 P633 — ET CA NE SUFFIT PAS DE LE FAIRE POUR LE FIL HEBERGE LUI-MEME.
    // `hosted_self()` corrigeait celui qui SORT du tourniquet ; il laissait
    // tels quels tous ceux qui y RESTENT. Or un tour ampute de six
    // coroutines est bien plus court : leurs delais finis, toujours comptes en
    // tours, expirent d'autant plus tot en temps reel. Mesure P630 : le pas
    // serveur passe de 1:1 avec l'image client a **29,6:1**, `svsTime` monte a
    // 1,87x le temps reel pendant que le rendu affiche 3 dessins, et le run
    // meurt sur « Server Disconnected - Timed out ».
    // ⇒ des qu'UNE SEULE coroutine quitte le tourniquet, un tour ne vaut plus
    // une milliseconde POUR PERSONNE. La porte est `g_any_hosted`, pas
    // `hosted_self()`. Cout en cooperatif : NUL, `g_any_hosted` y est faux.
    static const bool wfso_realms_env = bo2_env_on("BO2_WFSO_REALMS");
    const bool wfso_realms =
        wfso_realms_env || __builtin_expect(g_any_hosted, 0) || hosted_self();
    const uint64_t wfso_deadline_ns =
        (wfso_realms && timeout != 0xFFFFFFFFu && timeout != 0)
            ? bo2_mono_ns() + (uint64_t)timeout * 1000000ull : 0;
    // Finite timeouts are honored: ~1 coop_yield() rotation ≈ 1 ms of virtual
    // time, then WAIT_TIMEOUT. The render-pool workers poll their job event
    // with WFSO(h, 1) — treating that as INFINITE parked them forever (the
    // pre-Present livelock); a poller must time out and re-check its queue.
    GEvent* evp = ev_find(h);                         // P628/E3
    if (evp) {
        GEvent& E = *evp;         // reference, not iterator: another thread's
                                  // CreateEventA during our yield can rehash
                                  // g_events (iterator UB); references survive.
                                  // 🔴 C'est CE commentaire qui a donne E3 : la
                                  // stabilite des nœuds etait deja connue ici, il
                                  // ne manquait qu'un verrou sur la RECHERCHE.
        GThread* me = g_threads[g_cur];
        evop_note('W', h, retaddr, E.signaled);
        g_ev_wait[ev_slot(h)]++;
        if (sync_log(retaddr, h))
            fprintf(stderr, "[WFSO] h=0x%x timeout=0x%x ret=0x%08x sig=%d manual=%d tid=%d\n",
                    h, timeout, retaddr, E.signaled, E.manual, (int)g_cur);
        // BO2_PULSE_SEQ=1: also leave the wait if a PulseEvent happened at any
        // point while we were inside it. Value-checked, and OFF by default —
        // this changes scheduling, so it must be A/B-able against a journal
        // rather than switched on because it sounds right.
        static const bool pulse_seq_fix =
            bo2_getenv("BO2_PULSE_SEQ") && atoi(bo2_getenv("BO2_PULSE_SEQ")) != 0;
        const uint64_t pulse_seen = E.pulse_seq;
        // Le sondage cede une fois, signale ou non. Exclusions IDENTIQUES a
        // BO2_PREEMPT_W : un rappel de shim emprunte le State courant, et ceder
        // sous un verrou n'est une violation que tant que les sections critiques
        // n'excluent rien (avec BO2_CS_COOP=1 c'est la semantique Windows).
        if (timeout == 0 && wfso_poll_yield_on() && !in_shim_cb()) {
            const bool ulock = (g_cur >= 0 && g_cur < 64 && g_cs_depth[g_cur] > 0);
            if (!(ulock && !cs_coop_on())) {
                static uint64_t ny = 0;
                const uint64_t k = ++ny;
                if ((k & (k - 1)) == 0)   // puissances de deux : #N est le VRAI compte
                    fprintf(stderr, "[WFSO-POLL-YIELD #%llu] h=0x%x ret=0x%08x sig=%d tid=%d\n",
                            (unsigned long long)k, h, retaddr, E.signaled, (int)g_cur);
                coop_yield_from("WFSO-poll");
            }
        }
        long spins = 0;
        while (!E.signaled && !me->wake_token &&
               !(pulse_seq_fix && E.pulse_seq != pulse_seen)) {
            // FINITE timeouts only: an infinite waiter blocks, so it is on the
            // blocked list and `woken` already covers it.
            if (timeout != 0xFFFFFFFFu) E.polled = true;
            if (timeout == 0) { s.gpr.rax.dword = 0x102u; return; }   // WAIT_TIMEOUT
            if (timeout != 0xFFFFFFFFu) {
                const bool elapsed = wfso_realms
                    ? (bo2_mono_ns() >= wfso_deadline_ns)
                    : (spins >= (long)(timeout < 10000u ? timeout : 10000u));
                if (elapsed) { s.gpr.rax.dword = 0x102u; return; }      // finite timeout elapsed
            }
            if (spins == 0) g_ev_block[ev_slot(h)]++;
            if (spins == 0 && sync_log(retaddr, h ^ 0x80000000u, 2))
                fprintf(stderr, "[WFSO-block] h=0x%x timeout=0x%x ret=0x%08x tid=%d\n", h, timeout, retaddr, (int)g_cur);
            me->status = T_BLOCKED; me->wait_h = h;
            // A finite timeout must be able to expire while we are descheduled.
            me->wake_at = (timeout == 0xFFFFFFFFu) ? 0 : g_tick + 1;
            if (g_cur >= 0 && g_cur < 32) g_yield_handle[g_cur] = h;
            // 🎯 THE MIXER WAITS ON THE EVENT ITS OWN CALLBACK SETS. Measured:
            // the Sound Mix thread blocks here on handle 0x10032 with a 100 ms
            // timeout, and the ONLY signaller of that handle is
            // IXAudio2VoiceCallback::OnBufferEnd -> SetEvent, which on Windows
            // arrives from the XAudio2 engine thread. We deliver that callback
            // ourselves, from this very thread — so once it blocks here, the
            // thing that would wake it can no longer run, and it eats the full
            // timeout. (Same shape as the earlier deadlock from hooking the
            // flush to voice methods: the wake-up depended on already being
            // awake. This is that trap in a new costume.)
            //
            // So pump right here, while blocked: any buffer the device has
            // finished turns into an OnBufferEnd, which signals the event, and
            // the re-check below leaves the wait immediately.
            if (g_cur == g_rt_tid) bo2_xaudio_pump(&s);
            // 🎯 P648/C1 — l'autre moitie du Dekker de impl_SetEvent : T_BLOCKED est
            // publie ci-dessus, la barriere l'ordonne AVANT la relecture. La
            // relecture est atomique : en `bool` simple, le compilateur la
            // repliait sur la lecture de tete de boucle (rien entre les deux ne
            // peut, pour lui, modifier E). Et si elle trouve l'evenement signale,
            // on RESSORT de l'etat bloque : l'ancien `continue` laissait
            // T_BLOCKED/wait_h poses sur un fil qui reprenait le code invite.
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&E.signaled, __ATOMIC_RELAXED)) {
                static uint64_t nd = 0;
                const uint64_t k = ++nd;
                if ((k & (k - 1)) == 0)   // puissances de deux : #N est le VRAI compte
                    fprintf(stderr, "[WFSO-DEKKER #%llu] tid=%d h=0x%x ret=0x%08x : signale entre "
                                    "la publication T_BLOCKED et la relecture\n",
                            (unsigned long long)k, (int)g_cur, h, retaddr);
                me->status = T_RUN; me->wait_h = 0; me->wake_at = 0;
                continue;
            }
            if (me->hosted && me->hsem && hosted_self()) host_park_until(me, wfso_deadline_ns, "WFSO-event");  // C25
            else {
                if (wfso_deadline_ns && !hosted_self() && ordo_repos_niveau() >= 2) {   // P800
                    me->wfso_until_ns = wfso_deadline_ns;
                    ordo_repos_si_inactif(me, wfso_deadline_ns, "WFSO-event");
                }
                coop_yield_until("WFSO-event", wfso_deadline_ns);
                me->wfso_until_ns = 0;
            }
            me->status = T_RUN; me->wait_h = 0; me->wake_at = 0;
            if (++spins > 5000000) { s.gpr.rax.dword = 0x102u; return; }  // deadlock backstop
        }
        if (me->wake_token) { me->wake_token = false; s.gpr.rax.dword = 0; return; }  // pulsed awake
        if (pulse_seq_fix && E.pulse_seq != pulse_seen) {
            static uint32_t said = 0;
            if (said < 8) { said++;
                fprintf(stderr, "[PULSE-SEQ] tid=%d h=0x%x observed a pulse it would "
                                "have MISSED (seq %llu -> %llu)\n", (int)g_cur, h,
                        (unsigned long long)pulse_seen, (unsigned long long)E.pulse_seq); }
            s.gpr.rax.dword = 0; return;                                // WAIT_OBJECT_0
        }
        if (!E.manual) E.signaled = false;                             // auto-reset consume
        evop_note('w', h, retaddr, E.signaled);
        s.gpr.rax.dword = 0;                                            // WAIT_OBJECT_0
        return;
    }
    if (real_mutex_on()) {
        bool connu;
        { std::lock_guard<std::mutex> lk(g_mutex_mx); connu = g_mutexes.find(h) != g_mutexes.end(); }
        if (connu) {
            long spins = 0;
            for (;;) {
                int ow;
                {   // P661/C15 : test-et-prise indivisible entre fils hotes ; relache
                    // avant la cession plus bas (coop_yield_until).
                    std::lock_guard<std::mutex> lk(g_mutex_mx);
                    GMutex& M = g_mutexes[h];                               // re-find: yields can rehash
                    if (M.owner == -1 || M.owner == g_cur) { M.owner = g_cur; ++M.count; s.gpr.rax.dword = 0; return; }
                    ow = M.owner;
                    if (ow < 0 || (size_t)ow >= sched_nthreads() || !g_threads[ow] || g_threads[ow]->status == T_DONE) {
                        ++g_mutex_abandoned;
                        if (g_mutex_abandoned <= 8)
                            fprintf(stderr, "[MUTEX] h=0x%x was owned by tid %d, which is gone — WAIT_ABANDONED to tid %d\n", h, ow, (int)g_cur);
                        M.owner = g_cur; M.count = 1; s.gpr.rax.dword = 0x80u; return;
                    }
                }
                if (timeout == 0) { s.gpr.rax.dword = 0x102u; return; }
                // WFSO_REALMS couvre les TROIS sites, pas seulement les evenements :
                // BO2_REAL_MUTEX=1 est arme sur tous les bancs, donc celui-ci etait
                // actif dans les runs H et I qui ont semble refuter l'hypothese.
                if (timeout != 0xFFFFFFFFu) {
                    const bool elapsed = wfso_realms
                        ? (bo2_mono_ns() >= wfso_deadline_ns)
                        : (spins >= (long)(timeout < 10000u ? timeout : 10000u));
                    if (elapsed) { s.gpr.rax.dword = 0x102u; return; }
                }
                if (!spins && ++g_mutex_waits <= 6)
                    fprintf(stderr, "[MUTEX] tid %d waits for h=0x%x held by tid %d (from 0x%08x) — wait #%llu\n",
                            (int)g_cur, h, ow, retaddr, (unsigned long long)g_mutex_waits);
                g_threads[g_cur]->status = T_BLOCKED; g_threads[g_cur]->wait_h = h;
                g_threads[g_cur]->wake_at = g_tick + 1;                     // re-test every tick
                coop_yield_until("WFSO-mutex", wfso_deadline_ns);
                g_threads[g_cur]->status = T_RUN; g_threads[g_cur]->wait_h = 0; g_threads[g_cur]->wake_at = 0;
                if (++spins > 5000000) { s.gpr.rax.dword = 0x102u; return; }
            }
        }
    }
    const int tidx = th_find(h);                                        // P628/E3
    if (tidx >= 0) {                                                    // wait for a thread to finish
        // (l'index est une COPIE : la table peut rehacher pendant nos cessions)
        long spins = 0;
        while (g_threads[tidx]->status != T_DONE) {
            if (timeout == 0) { s.gpr.rax.dword = 0x102u; return; }
            if (timeout != 0xFFFFFFFFu) {
                const bool elapsed = wfso_realms
                    ? (bo2_mono_ns() >= wfso_deadline_ns)
                    : (spins >= (long)(timeout < 10000u ? timeout : 10000u));
                if (elapsed) { s.gpr.rax.dword = 0x102u; return; }
            }
            g_threads[g_cur]->status = T_BLOCKED;
#if defined(BO2_TARGET_MPS) || defined(BO2_TARGET_ZMS)
            // Z-NATIF-09x : une jointure INFINIE est re-testee a chaque tour, comme la branche mutex. Avec
            // wake_at=0 personne ne reelit le joigneur (le tourniquet n'elit que des T_RUN, la fin d'un fil ne
            // reveille pas ses attendeurs) : tid 0 gele sur le fil DNS 0xadaa30 (run 175500).
            g_threads[g_cur]->wake_at = g_tick + 1;
#else
            g_threads[g_cur]->wake_at = (timeout == 0xFFFFFFFFu) ? 0 : g_tick + 1;
#endif
            coop_yield_until("WFSO-thread", wfso_deadline_ns);
            g_threads[g_cur]->status = T_RUN; g_threads[g_cur]->wake_at = 0;
            if (++spins > 5000000) { s.gpr.rax.dword = 0x102u; return; }
        }
        s.gpr.rax.dword = 0; return;
    }
    if (sync_log(retaddr, h, 3))
        fprintf(stderr, "[WFSO-unk] h=0x%x timeout=0x%x ret=0x%08x tid=%d -> WAIT_OBJECT_0\n",
                h, timeout, retaddr, (int)g_cur);
    s.gpr.rax.dword = 0;                                               // unknown handle -> signaled
}

static void impl_ExitThread(State& s) {
    uint32_t code = win32_arg(s, 0);
    pthread_exit((void*)(uintptr_t)code);
    // never reached
}

// --- TIME ---
//
// The guest's only sense of time is the host wall clock, so making the guest
// FASTER does not make the game faster -- it changes the ratio between guest
// work and the wall millisecond. Every "work until N ms have elapsed" loop then
// does more per slice, and since the scheduler never preempts, the interleaving
// of coroutines at the 8 blocking primitives moves with it.
//
// BO2_CLOCK_SCALE=k makes the guest perceive k milliseconds per real one:
// t0 + (now - t0) * k, one multiply at the single point every monotonic clock
// goes through. k > 1 reproduces a SLOWER guest's work-per-perceived-ms while
// keeping the fast write barrier. Applied to the three MONOTONIC clocks only;
// GetSystemTimeAsFileTime stays real calendar time.
// ⚠️ `extern "C"` est une LINKAGE-SPECIFICATION : illegale au niveau bloc. Elle
// doit etre ici, pas dans le corps de bo2_mono_ns. (Deuxieme fois ce soir.)
extern "C" double bo2_w32_burn_ns_per_store(void);

extern "C" uint64_t bo2_mono_ns(void) {
    static double   scale = 0.0;                 // 0 = not yet read
    static uint64_t t0    = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (scale == 0.0) {
        const char* e = bo2_getenv("BO2_CLOCK_SCALE");
        const double k = e ? atof(e) : 1.0;
        scale = (k > 0.0) ? k : 1.0;
        t0    = now;
        if (scale != 1.0)
            fprintf(stderr, "[CLOCK-SCALE] %.3f perceived ms per real ms\n", scale);
    }
    // BO2_CLOCK_DEBURN=1 -- RETRANCHER DE L'HORLOGE EXACTEMENT CE QUE `BURN` Y AJOUTE.
    //
    // Le raisonnement. `BO2_W32_BURN=N` n'ajoute QUE du temps reel : N iterations
    // vides apres chaque ecriture invitee, aucune semantique. Il fait pourtant
    // passer un run qui echoue 7 fois sur 7 sans lui. L'ordonnanceur etant
    // strictement cooperatif, ce temps ne peut pas changer l'entrelacement ; il
    // faut donc qu'une entree TEMPS REEL atteigne l'invite. Apres elimination
    // mesuree de la fenetre, du drain de messages, de l'audio, de CryptGenRandom
    // et de l'E/S fichier synchrone, il ne reste que la lecture d'horloge.
    //
    // Pourquoi CETTE sonde plutot qu'une horloge virtuelle. La version calibree
    // sur les commutations cooperatives a echoue sur son propre temoin : mesuree
    // pendant le chargement (1 696 commutations en 2 s), sa cadence n'a rien a
    // voir avec celle du jeu, et l'horloge s'est emballee. Ici il n'y a RIEN a
    // calibrer sur l'invite : on soustrait une duree qu'on a comptee.
    //
    // Le test est symetrique et falsifiable :
    //   rapide + BURN=30                  -> REUSSITE connue 3/3 (temoin)
    //   rapide + BURN=30 + CLOCK_DEBURN=1 -> si l'horloge EST le canal, ECHEC.
    // Un ECHEC ici est la preuve ; une REUSSITE tue l'hypothese de l'horloge.
    //
    // Monotonie garantie explicitement : `now` et le temps brule croissent tous
    // les deux, leur difference non. Une horloge qui recule casserait tout dt.
    {
        extern uint64_t g_w32_burn_stores;
        static int      deburn = -1;
        static double   ns_per_iter = 0.0;
        static uint64_t last = 0;
        if (deburn < 0) {
            deburn = bo2_env_on("BO2_CLOCK_DEBURN") ? 1 : 0;
            if (deburn) {
                // Cout d'une iteration, mesure sur la boucle EXACTE du barriere.
                ns_per_iter = bo2_w32_burn_ns_per_store();
                fprintf(stderr, "[CLOCK-DEBURN] %.3f ns per burned STORE (same loop shape as "
                                "the barrier's); the guest's monotonic clock will run with "
                                "BO2_W32_BURN's cost removed\n", ns_per_iter);
            }
        }
        if (deburn) {
            const uint64_t burned = (uint64_t)((double)g_w32_burn_stores * ns_per_iter);
            uint64_t v = (now - t0 > burned) ? (now - burned) : t0;
            if (v < last) v = last; else last = v;
            static uint64_t next_report = 0;
            if (now - t0 > next_report) {
                next_report = (now - t0) + 30000000000ull;   // toutes les 30 s reelles
                // La PART est le controle de sante de la sonde : l'arithmetique des
                // temps mesures en exige ~81 % a BURN=30. Si elle affiche 27 %, la
                // calibration est fausse et le resultat du run ne vaut rien.
                fprintf(stderr, "[CLOCK-DEBURN] wall=%.1fs burned=%.1fs (%.0f%% du mur) "
                                "perceived=%.1fs (%llu stores)\n",
                        (double)(now - t0) / 1e9, (double)burned / 1e9,
                        100.0 * (double)burned / (double)(now - t0 ? now - t0 : 1),
                        (double)(v - t0) / 1e9, (unsigned long long)g_w32_burn_stores);
            }
            if (scale == 1.0) return v;
            return t0 + (uint64_t)((double)(v - t0) * scale);
        }
    }
    if (scale == 1.0) return now;
    return t0 + (uint64_t)((double)(now - t0) * scale);
}

static void impl_GetTickCount(State& s) {
    s.gpr.rax.dword = (uint32_t)(bo2_mono_ns() / 1000000ull);
    win32_stdcall_ret(s, 0);
}

static void impl_QueryPerformanceCounter(State& s) {
    // arg0 = LARGE_INTEGER* (pointer to 64-bit int)
    uint32_t ptr = win32_arg(s, 0);
    uint64_t count = bo2_mono_ns();
    win32_mem_write32(ptr, (uint32_t)count);
    win32_mem_write32(ptr + 4, (uint32_t)(count >> 32));
    s.gpr.rax.dword = 1; // BOOL success
    win32_stdcall_ret(s, 1);
}

static void impl_QueryPerformanceFrequency(State& s) {
    uint32_t ptr = win32_arg(s, 0);
    uint64_t freq = 1000000000ULL; // 1 GHz (nanosecond precision)
    win32_mem_write32(ptr, (uint32_t)freq);
    win32_mem_write32(ptr + 4, (uint32_t)(freq >> 32));
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 1);
}

static void impl_GetSystemTimeAsFileTime(State& s) {
    // arg0 = FILETIME* (pointer to 64-bit)
    uint32_t ptr = win32_arg(s, 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    // FILETIME = 100ns intervals since 1601-01-01
    // Unix epoch (1970) = 11644473600 seconds after 1601
    uint64_t unix_100ns = (uint64_t)ts.tv_sec * 10000000ULL + ts.tv_nsec / 100;
    uint64_t filetime = unix_100ns + 116444736000000000ULL;
    win32_mem_write32(ptr, (uint32_t)filetime);
    win32_mem_write32(ptr + 4, (uint32_t)(filetime >> 32));
    win32_stdcall_ret(s, 1);
}

// GetSystemTime(lpSystemTime) — void, stdcall 1 arg. Heure UTC en SYSTEMTIME (8 WORD : annee, mois,
// jour de semaine, jour, heure, minute, seconde, millisecondes). Absente de l'IAT de t6sp ; zombies
// l'appelle au demarrage et l'import non implemente l'arretait. Meme horloge que GetSystemTimeAsFileTime.
static void impl_GetSystemTime(State& s) {
    const uint32_t ptr = win32_arg(s, 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    gmtime_r(&ts.tv_sec, &t);
    const auto paire = [](int lo, int hi) { return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16); };
    win32_mem_write32(ptr + 0, paire(t.tm_year + 1900, t.tm_mon + 1));
    win32_mem_write32(ptr + 4, paire(t.tm_wday, t.tm_mday));
    win32_mem_write32(ptr + 8, paire(t.tm_hour, t.tm_min));
    win32_mem_write32(ptr + 12, paire(t.tm_sec, (int)(ts.tv_nsec / 1000000)));
    win32_stdcall_ret(s, 1);
}


// --- MEMORY ---
static void impl_VirtualAlloc(State& s) {
    // VirtualAlloc(addr, size, type, protect) — stdcall 4 args. Serve from the
    // guest arena; never return a host mmap pointer. Honor a requested guest
    // address (g_mem backs any address); otherwise hand out a fresh page.
    uint32_t addr = win32_arg(s, 0);
    uint32_t size = win32_arg(s, 1);
    heap_trace_sp(s.gpr.rsp.dword, size, "VirtualAlloc");
    s.gpr.rax.dword = addr ? addr : guest_alloc(size, 0x1000);
    win32_stdcall_ret(s, 4);
}

static void impl_VirtualFree(State& s) {
    // VirtualFree(addr, size, type) — stdcall 3 args. MEM_RELEASE (0x8000) frees
    // the whole reservation; MEM_DECOMMIT alone leaves the address reserved, so
    // only the former returns the block. Must NOT munmap the arg — it is a guest
    // address.
    if (win32_arg(s, 2) & 0x8000u) guest_free(win32_arg(s, 0));
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 3);
}

static void impl_VirtualQuery(State& s) {
    // VirtualQuery(lpAddress, lpBuffer, dwLength) — stdcall 3 args. Fill a
    // MEMORY_BASIC_INFORMATION (28 bytes on win32) describing the queried page
    // as committed private read/write. A region-walk (addr += RegionSize) must
    // terminate, so we report one large region up to the top of user space.
    uint32_t addr = win32_arg(s, 0);
    uint32_t buf  = win32_arg(s, 1);
    uint32_t len  = win32_arg(s, 2);
    if (len < 28) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 3); return; }
    uint32_t page = addr & ~0xFFFu;
    uint32_t region_end = 0x7FFF0000u;                 // top of 32-bit user space
    uint32_t region = (page < region_end) ? (region_end - page) : 0x1000u;
    win32_mem_write32(buf + 0,  page);        // BaseAddress
    win32_mem_write32(buf + 4,  0x00010000);  // AllocationBase
    win32_mem_write32(buf + 8,  0x04);        // AllocationProtect = PAGE_READWRITE
    win32_mem_write32(buf + 12, region);      // RegionSize
    win32_mem_write32(buf + 16, 0x1000);      // State = MEM_COMMIT
    win32_mem_write32(buf + 20, 0x04);        // Protect = PAGE_READWRITE
    win32_mem_write32(buf + 24, 0x20000);     // Type = MEM_PRIVATE
    s.gpr.rax.dword = 28;                      // bytes returned
    win32_stdcall_ret(s, 3);
}

// --- FILE I/O ---
// We map Win32 file handles to POSIX file descriptors directly.
// (This works because both are small integers in practice.)
// Game install dir. Guest file paths are Windows-style ('\'-separated, often
// relative to the exe dir); resolve them INTO this dir so the title's config /
// asset loads hit the real files: strip any drive letter + leading separators,
// translate '\'->'/', prepend GAME_DIR.
// Player's game folder: BO2_GAME_DIR, else the line scripts/install_steam.sh writes to ~/.bo2-native/game_dir (or the
// older ~/.bo2-native/jeu), else /Users/Shared/Call of Duty - Black Ops 2.
static const std::string g_game_dir = [] {
    if (const char* e = bo2_getenv("BO2_GAME_DIR")) return std::string(e);
    std::string d = "/Users/Shared/Call of Duty - Black Ops 2";
    if (const char* h = getenv("HOME")) {
        FILE* f = fopen((std::string(h) + "/.bo2-native/game_dir").c_str(), "r");
        if (!f) f = fopen((std::string(h) + "/.bo2-native/jeu").c_str(), "r");
        if (f) {
            char l[1024];
            if (fgets(l, sizeof l, f)) { l[strcspn(l, "\n")] = 0; if (*l) d = l; }
            fclose(f);
        }
    }
    fprintf(stderr, "[JEU] dossier du jeu : %s\n", d.c_str());
    return d;
}();
#define GAME_DIR g_game_dir.c_str()
// The ipak index reader needs the same root the path mapper uses.
extern "C" const char* win32_game_dir() { return GAME_DIR; }
static void win32_resolve_path(uint32_t guest_ptr, char* out, size_t outsz) {
    char raw[1024]; int i = 0;
    for (; i < 1023; i++) { char c = (char)win32_mem_read8(guest_ptr + i); raw[i] = c; if (!c) break; }
    raw[1023] = 0;
    const char* p = raw;
    if (p[0] && p[1] == ':') p += 2;              // strip drive "X:"
    while (*p == '\\' || *p == '/') p++;           // strip leading separators
    size_t n = snprintf(out, outsz, "%s/", GAME_DIR);
    for (; *p && n + 1 < outsz; p++) out[n++] = (*p == '\\') ? '/' : *p;
    out[n] = 0;
}

// .webm file descriptors, so ReadFile can report on THEM specifically.
//
// The tester watching the screen reported black flashes at the campaign start — the
// cinematic surface IS being presented, it just has no picture. That is a different
// statement from "nothing happens", which is all the decoder log could say, and it
// makes the next question exact: is the file ever READ? A dedicated counter is
// required because the shared per-fd read budget in impl_ReadFile has already
// produced one false "not one byte is ever read" in this project.
static int   g_webm_fd[8];
static char  g_webm_nm[8][64];
static int   g_webm_n;
extern "C" int bo2_is_webm_fd(int fd, const char** name) {
    for (int i = 0; i < g_webm_n; i++)
        if (g_webm_fd[i] == fd) { if (name) *name = g_webm_nm[i]; return 1; }
    return 0;
}

extern "C" void bo2_vpx_new_video(void);
static void impl_CreateFileA(State& s) {
    // CreateFileA(filename, access, share, sa, disp, flags, template) — stdcall 7 args
    uint32_t filename_ptr = win32_arg(s, 0);
    // Reset the per-video decoder frame counter on every .webm open — see the note
    // in vpx_native.cpp. Unconditional: it must not depend on a debug flag being set,
    // because the flaw it fixes silently invalidated a headline conclusion.
    {
        char wn[128]; int wk = 0;
        while (wk < 127) { uint8_t c = win32_mem_read8(filename_ptr + wk);
                           if (c < 0x20 || c > 0x7e) break; wn[wk++] = (char)c; }
        wn[wk] = 0;
        if (wk > 5 && !strcmp(wn + wk - 5, ".webm")) bo2_vpx_new_video();
    }
    // BO2_WEBM=1 — WHO opens each .webm, seen from the ONE place every video passes.
    //
    // There is already a stack scan of this kind on the .webm path, but it lives in
    // the zone opener (sub_5dd070), and the measurement that matters is precisely
    // that the prologue does NOT come through there: the boot logos do and decode,
    // the campaign prologue arrives via the CRT and never decodes a single frame,
    // in the same run, with the player proven alive. A probe that cannot see the
    // failing case is no use for the comparison, so put the same scan HERE, where
    // both arrive, and let the two call chains be diffed directly.
    if (bo2_getenv("BO2_WEBM")) {
        char nm[128]; int k = 0;
        while (k < 127) { uint8_t c = win32_mem_read8(filename_ptr + k);
                          if (c < 0x20 || c > 0x7e) break; nm[k++] = (char)c; }
        nm[k] = 0;
        if (k > 5 && !strcmp(nm + k - 5, ".webm")) {
            fprintf(stderr, "[WEBM-CREATEFILE] \"%s\" tid=%d — guest stack:\n",
                    nm, win32_current_tid());
            int shown = 0;
            for (uint32_t o = 0; o < 0x400 && shown < 20; o += 4) {
                const uint32_t v = win32_mem_read32(s.gpr.rsp.dword + o);
                if (v >= 0x401000u && v < 0xa31000u)          // inside .text
                    fprintf(stderr, "[WEBM-CREATEFILE]   +%03x -> 0x%08x\n", o, v), shown++;
            }
        }
    }
    uint32_t access = win32_arg(s, 1);
    uint32_t disp = win32_arg(s, 4);
    char filename[1200];
    win32_resolve_path(filename_ptr, filename, sizeof(filename));
    int flags_posix = 0;
    if (access & 0x80000000) { // GENERIC_READ
        flags_posix = (access & 0x40000000) ? O_RDWR : O_RDONLY; // GENERIC_WRITE
    } else if (access & 0x40000000) {
        flags_posix = O_WRONLY;
    }
    // dwCreationDisposition: CREATE_NEW=1, CREATE_ALWAYS=2, OPEN_EXISTING=3,
    // OPEN_ALWAYS=4, TRUNCATE_EXISTING=5. (The previous mapping was wrong — it
    // mapped OPEN_EXISTING to O_CREAT|O_EXCL, so EVERY existing file the title
    // opened for read failed: localization.txt, the zone/*.ff fastfiles, ...)
    if (disp == 1) flags_posix |= O_CREAT | O_EXCL;        // CREATE_NEW
    else if (disp == 2) flags_posix |= O_CREAT | O_TRUNC;  // CREATE_ALWAYS
    else if (disp == 4) flags_posix |= O_CREAT;            // OPEN_ALWAYS
    else if (disp == 5) flags_posix |= O_TRUNC;            // TRUNCATE_EXISTING
    // disp == 3 (OPEN_EXISTING): no extra flags — open existing, fail if absent.
    int fd = open(filename, flags_posix, 0644);
    // ⚠️ NEVER HAND THE GUEST A RECYCLED DESCRIPTOR NUMBER.
    //
    // This shim returns the raw POSIX fd as the guest HANDLE, and POSIX always
    // hands back the LOWEST free descriptor. Windows does not recycle HANDLE
    // values anywhere near as eagerly, so the guest is entitled to assume a
    // freshly opened file does not collide with one it thinks it still holds.
    //
    // MEASURED 2026-08-07, and it is why the campaign zone never loads:
    //     [CreateFileA] ".../video/intro.webm"      -> ok (fd=18)
    //     [STREAM] slot 38 "\video\intro.webm" handle=18 inuse=1   <- still live
    //     [CreateFileA] ".../zone/all/afghanistan.ff" -> ok (fd=18) <- SAME number
    // and from there the streaming worker spins on a request with
    // file=4294967295 (-1): 224,041 callback dispatches against 3,046 real
    // reads, and afghanistan.ff is opened and never read. In one run fd 18 was
    // reused SEVEN times across 36 opens (18 distinct numbers).
    //
    // fcntl(F_DUPFD) returns the lowest free descriptor >= the floor, so
    // bumping the floor makes handles monotonic and non-reusable. It costs
    // nothing: 36 opens in a whole run against a soft limit of 1,048,576.
    // BO2_ALLOW_FD_REUSE=1 restores the old behaviour for A/B comparison.
    static int g_fd_floor = 64;
    if (fd >= 0 && !bo2_getenv("BO2_ALLOW_FD_REUSE")) {
        const int hi = fcntl(fd, F_DUPFD, g_fd_floor);
        if (hi >= 0) {
            close(fd);
            fd = hi;
            g_fd_floor = hi + 1;
        }
        // On failure keep the original fd: a recycled descriptor still works,
        // it merely risks the collision above. Degrading is better than losing
        // the open entirely.
    }
    fprintf(stderr, "[CreateFileA] \"%s\" -> %s (fd=%d ret=0x%08x)\n",
            filename, fd < 0 ? "FAIL" : "ok", fd, win32_mem_read32(s.gpr.rsp.dword));
    // WHO OPENED THIS VIDEO, one level above the immediate caller. The prologue's
    // ONLY trace in a whole campaign-starting run is a single open from the
    // CRT-ish path at 0x98030b — nothing plays it — so "what was that open for"
    // is the last open thread.
    //
    // ⚠️ NOT A STACK SCAN (§9 forbids deriving a chain from one). The caller at
    // 0x9802f0.. is an ebp-framed routine — it addresses its locals as
    // [ebp-0x10], [ebp-0x18], [ebp-0x30] — so guest [ebp+4] IS its return
    // address by the frame's own construction, and [ebp] its saved ebp. Two
    // frames, read from the frame pointer, and nothing beyond that.
    if (bo2_getenv("BO2_VIDOPEN")) {
        if (const char* dot = std::strrchr(filename, '.')) {
            if (std::strcmp(dot, ".webm") == 0) {
                // Walk the SAVED-EBP CHAIN, at most 8 frames, and stop at the
                // first return address below 0x950000 — that is the first frame
                // outside the CRT, i.e. the engine function that asked for this
                // file. Bounded, and it follows the frame pointer rather than
                // sifting the stack for plausible values.
                char chain[256]; int n = 0;
                uint32_t ebp = s.gpr.rbp.dword;
                for (int f = 0; f < 8 && ebp >= 0x1000u && ebp < 0xF0000000u; f++) {
                    const uint32_t ret = win32_mem_read32(ebp + 4);
                    if (ret < 0x401000u || ret >= 0xa31000u) break;
                    n += snprintf(chain + n, sizeof chain - n, " %08x", ret);
                    if (ret < 0x950000u) { n += snprintf(chain + n, sizeof chain - n, " <ENGINE"); break; }
                    const uint32_t next = win32_mem_read32(ebp);
                    if (next <= ebp) break;                 // frames grow upward
                    ebp = next;
                }
                chain[n] = 0;
                fprintf(stderr, "[VIDOPEN] \"%s\" immediate=0x%08x ebp-chain:%s tid=%d\n",
                        std::strrchr(filename, '/') ? std::strrchr(filename, '/') + 1 : filename,
                        win32_mem_read32(s.gpr.rsp.dword), chain, (int)g_cur);
            }
        }
    }
    // ❌ A GATE PROBE LIVED HERE AND MEASURED NOTHING; the negative is worth
    // keeping so nobody re-derives it. sub_6743a0 (the engine's file opener) has
    // two callers, 0x5dd14b and 0x80d71f, and 0x80d71f sits behind
    //     mov esi, 0x291f2b0
    //     cmp byte [esi-0x14], bl ; je skip     -> 0x291f29c
    //     cmp byte [esi-0x13], bl ; je skip     -> 0x291f29d
    // which reads like a streaming gate. It is not the one in play: printing
    // both bytes plus the name buffer at [esi-0x118] on every .webm open gave
    // 0 / 0 / "" EVERY TIME — including for the logo videos that decode
    // perfectly. So the videos come through the OTHER caller, 0x5dd14b, which
    // is the zone-load path (_sub_5dd070 = _bo2_zone_load), and `bl` was never
    // shown to be zero either. Two assumptions, both wrong, caught by the probe
    // rather than by a conclusion. See the [ZONE-LOAD] asymmetry instead.
    if (fd < 0) {
        s.gpr.rax.dword = 0xFFFFFFFF; // INVALID_HANDLE_VALUE
        t_last_error = errno;
    } else {
        s.gpr.rax.dword = (uint32_t)fd;
    }
    win32_stdcall_ret(s, 7);
}

// Real GetFileAttributesA (was STUB1 -> 0, which falsely told the title every
// file exists). stat the resolved path: directory / normal / INVALID.
static void impl_GetFileAttributesA(State& s) {
    char path[1200];
    win32_resolve_path(win32_arg(s, 0), path, sizeof(path));
    struct stat st;
    uint32_t attr = (stat(path, &st) == 0)
        ? ((st.st_mode & S_IFDIR) ? 0x10u : 0x80u)   // DIRECTORY : NORMAL
        : 0xFFFFFFFFu;                                 // INVALID_FILE_ATTRIBUTES
    fprintf(stderr, "[GetFileAttributesA] \"%s\" -> 0x%x (ret=0x%08x) [gate 1013808=%u ff348c=%02x]\n",
            path, attr, win32_mem_read32(s.gpr.rsp.dword),
            win32_mem_read8(0x1013808), win32_mem_read8(0xff348c));
    s.gpr.rax.dword = attr;
    win32_stdcall_ret(s, 1);
}

// File mappings: the title reads files (localization.txt, zone/*.ff fastfiles)
// via CreateFileMapping + MapViewOfFile, NOT ReadFile. Implement for real — the
// mapping remembers the fd; MapViewOfFile reads the file into a fresh guest
// arena region and returns its guest address, so mapped reads see real content.
// `base` is non-zero for an ANONYMOUS (pagefile-backed) mapping — the
// CreateFileMappingA(INVALID_HANDLE_VALUE, …) form, which has no fd. Both t6mp
// and t6zm need it before they will run: their WinMain gate does
//   CreateSemaphoreA("STEAM_DIPC_CONSUME") / ("SREAM_DIPC_PRODUCE")
//   CreateFileMappingA(-1, …, "…")  ->  MapViewOfFile  ->  sub_4e12b0(view, 1)
// and returns sub_4e12b0's result. With no anonymous support MapViewOfFile
// returned 0, the gate returned false, and WinMain took its early exit at
// 0x5f3416 (`xor eax,eax; ret 0x10`) — the game shut down cleanly with status 0
// and no error anywhere, which is why it looked like it had simply finished.
struct FileMapping { int fd; uint32_t base; uint32_t size; };
static std::unordered_map<uint32_t, FileMapping> g_file_mappings;

static void impl_CreateFileMappingA(State& s) {
    // (hFile, lpAttr, flProtect, dwMaxHigh, dwMaxLow, lpName) — 6 args
    uint32_t hFile = win32_arg(s, 0);
    uint32_t h = ++t_fake_handle_counter;
    // INVALID_HANDLE_VALUE (and 0) mean "back this with the pagefile", i.e. it is
    // shared memory with no file behind it. Allocate it from the guest arena now
    // and zero it, so every MapViewOfFile of this handle returns the same view.
    if (hFile == 0xFFFFFFFFu || hFile == 0) {
        uint32_t size = win32_arg(s, 4);          // dwMaximumSizeLow
        if (size == 0) size = 0x1000;
        uint32_t base = guest_alloc(size, 0x1000);
        for (uint32_t i = 0; i < size; i += 4) win32_mem_write32(base + i, 0);
        g_file_mappings[h] = { -1, base, size };
    } else {
        g_file_mappings[h] = { (int)hFile, 0, 0 };
    }
    s.gpr.rax.dword = h;
    win32_stdcall_ret(s, 6);
}
static void impl_MapViewOfFile(State& s) {
    // (hMapping, dwAccess, offHigh, offLow, dwNumberOfBytesToMap) — 5 args
    uint32_t hMapping = win32_arg(s, 0);
    uint32_t offLow = win32_arg(s, 3);
    uint32_t size = win32_arg(s, 4);
    auto it = g_file_mappings.find(hMapping);
    // Anonymous mapping: hand back the guest arena block allocated at create
    // time, offset by offLow. No fd is involved and none is needed.
    if (it != g_file_mappings.end() && it->second.base) {
        uint32_t off = (offLow < it->second.size) ? offLow : 0;
        s.gpr.rax.dword = it->second.base + off;
        win32_stdcall_ret(s, 5);
        return;
    }
    int fd = (it != g_file_mappings.end()) ? it->second.fd : -1;
    struct stat st;
    if (fd <= 0 || (uint32_t)fd == 0xFFFFFFFFu || fstat(fd, &st) != 0) {
        s.gpr.rax.dword = 0; win32_stdcall_ret(s, 5); return;
    }
    uint32_t avail = (offLow < (uint32_t)st.st_size) ? (uint32_t)st.st_size - offLow : 0;
    uint32_t maplen = size ? size : avail;
    if (maplen > avail) maplen = avail;
    uint32_t base = guest_alloc(maplen ? maplen : 1, 0x1000);
    uint8_t* tmp = (uint8_t*)malloc(maplen ? maplen : 1);
    lseek(fd, offLow, SEEK_SET);
    ssize_t n = (maplen ? read(fd, tmp, maplen) : 0);
    for (ssize_t i = 0; i < n; i++) win32_mem_write8(base + i, tmp[i]);
    free(tmp);
    fprintf(stderr, "[MapViewOfFile] fd=%d off=%u size=%u -> 0x%08x (%zd bytes)\n",
            fd, offLow, maplen, base, n);
    s.gpr.rax.dword = base;
    win32_stdcall_ret(s, 5);
}

static void impl_ReadFile(State& s) {
    // ReadFile(handle, buf, count, &bytes_read, overlap) — stdcall 5 args
    uint32_t fd = win32_arg(s, 0);
    uint32_t buf = win32_arg(s, 1);
    uint32_t count = win32_arg(s, 2);
    uint32_t bytes_read_ptr = win32_arg(s, 3);
    // 🎯 READ STRAIGHT INTO GUEST MEMORY. This used to malloc a bounce buffer and
    // then store the result ONE BYTE AT A TIME through win32_mem_write8, which
    // runs the write watchpoint, the IAT window, the fastfile ring bookkeeping and
    // the MT pool veto on every byte. That is every fastfile and every .ipak chunk
    // in the game: angola alone is 55 MB, base.ipak is 2.5 GB. Same reasoning and
    // the same shared predicate as the inflate shim — guest memory is one flat
    // 4 GiB mapping, so `win32_guest_ptr(buf)` IS the destination, and only a
    // destination overlapping a hooked window (or BO2_WATCH_WRITE) needs the loop.
    // ⚠️ `tmp` is still what the [ReadFile] line below prints its first 8 bytes
    // from, so on the direct path point it at the guest buffer rather than
    // leaving it dangling — printing a freed/never-filled buffer is exactly the
    // kind of quiet lie this file keeps a traps list about.
    uint8_t* tmp = nullptr;
    ssize_t n;
    const bool direct = count && !win32_bulk_dst_hooked(buf, count);
    if (direct) {
        tmp = win32_guest_ptr(buf);
        n = read(fd, tmp, count);
    } else {
        uint8_t* bounce = (uint8_t*)malloc(count);
        n = read(fd, bounce, count);
        for (ssize_t i = 0; i < n; i++) win32_mem_write8(buf + i, bounce[i]);
        tmp = bounce;
    }
    // PER-FD budget, not one global cap. A single 96-line global budget was
    // spent entirely on the fastfiles and .ipaks before the sound banks were
    // ever read, and the resulting silence in the log read as "not one byte is
    // ever read from a sound bank" — which I believed, wrote up, and committed.
    // The reads were happening all along. A cap that one class of caller can
    // exhaust cannot answer "does X ever happen?" for any other class.
    // ⚠️ THE PER-FD CAP MUST COVER EVERY FD WE CAN HAND OUT. This array was 64
    // entries with a global fallback for anything higher — and that fallback is
    // exhausted within the first second of a run. On 2026-08-07 the descriptor
    // fix above (monotonic fcntl(F_DUPFD) from a floor of 64) moved EVERY fd to
    // >= 64, so this log silently stopped reporting reads for every file in the
    // game, and I then read "0 reads on fd 98" as proof the campaign zone was
    // never read. A cap keyed on a range the allocator no longer produces
    // reports silence for everything, which is indistinguishable from the very
    // event being measured. Size it from the same floor the allocator uses.
    static int s_rf_fd[4096] = {};
    static uint64_t s_rf_total = 0;
    s_rf_total++;
    const bool s_show = (fd >= 0 && fd < 4096) ? (s_rf_fd[fd]++ < 6)
                                               : (s_rf_total < 96);
    // 🎯 AN UNCAPPED PER-FD TALLY, because the capped line above IS the reason
    // four sessions hunted a zone stall that never existed.
    //
    // The cap is 6 lines per fd. scripts/campaign_run.sh measures zone progress
    // by COUNTING THOSE LINES, so every large fastfile reports "read 6 chunks"
    // forever — and 6 was then written into the handoff as "angola.ff stops at 6
    // of 8,744 chunks", made the headline blocker, and sent three sessions into
    // the streaming pipeline. Measured on the same build the day this was found:
    // 6 [ReadFile] lines for fd 97, and 8,740 of angola's 8,744 blocks actually
    // inflated. The engine was never stuck.
    //
    // A capped probe cannot answer "how far did this get". Keep the readable
    // per-read lines capped (they are for eyeballing), and carry a tally that
    // never stops counting for the question the harness actually asks.
    if (fd >= 0 && fd < 4096 && n > 0) {
        static uint64_t s_rf_n[4096] = {}, s_rf_bytes[4096] = {};
        s_rf_n[fd]++; s_rf_bytes[fd] += (uint64_t)n;
        if ((s_rf_n[fd] % 256) == 0)
            fprintf(stderr, "[READ-TALLY] fd=%u reads=%llu bytes=%llu (%llu MiB)\n",
                    fd, (unsigned long long)s_rf_n[fd],
                    (unsigned long long)s_rf_bytes[fd],
                    (unsigned long long)(s_rf_bytes[fd] >> 20));
    }
    if (s_show)
        fprintf(stderr, "[ReadFile] fd=%u dst=0x%08x count=%u foff=0x%llx -> n=%zd tid=%d first=\"%.8s\"\n",
                fd, buf, count,
                (unsigned long long)(lseek(fd, 0, SEEK_CUR) - (n > 0 ? n : 0)),
                n, (int)g_cur, (n > 0) ? (char*)tmp : "");
    if (!direct) free(tmp);      // on the direct path `tmp` IS guest memory
    if (bytes_read_ptr) {
        win32_mem_write32(bytes_read_ptr, (uint32_t)n);
    }
    s.gpr.rax.dword = (n >= 0) ? 1 : 0;
    win32_stdcall_ret(s, 5);
}

static void impl_WriteFile(State& s) {
    uint32_t fd = win32_arg(s, 0);
    uint32_t buf = win32_arg(s, 1);
    uint32_t count = win32_arg(s, 2);
    uint32_t bytes_written_ptr = win32_arg(s, 3);
    uint8_t* tmp = (uint8_t*)malloc(count);
    for (uint32_t i = 0; i < count; i++) {
        tmp[i] = win32_mem_read8(buf + i);
    }
    ssize_t n = write(fd, tmp, count);
    free(tmp);
    if (bytes_written_ptr) {
        win32_mem_write32(bytes_written_ptr, (uint32_t)n);
    }
    s.gpr.rax.dword = (n >= 0) ? 1 : 0;
    win32_stdcall_ret(s, 5);
}

static void impl_CloseHandle(State& s) {
    uint32_t handle = win32_arg(s, 0);
    // Z-NATIF-12e : une poignee de fil fermee n'existe plus ; c'est ce qui autorise spawn_thread a reprendre la
    // case une fois le fil termine (une seule poignee par fil : spawn_thread est le seul a en inscrire).
    if (const int idx = th_find(handle); idx > 0) {
        { std::unique_lock<std::shared_mutex> lk(g_ev_mx); g_thread_handle.erase(handle); }
        g_threads[idx]->handle_closed = true;
        s.gpr.rax.dword = 1;
        win32_stdcall_ret(s, 1);
        return;
    }
    if (handle && handle != 0xFFFFFFFF && handle != 0xFFFFFFFE) {
        close((int)handle);
    }
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 1);
}

// GetFileSize(handle, lpFileSizeHigh) — the HIGH dword is an out-param and BO2
// passes it: zone/all/base.ipak is 2493 MB, so a caller that only trusts the low
// dword cannot tell a >2 GB file from a truncated one.
static void impl_GetFileSize(State& s) {
    uint32_t fd = win32_arg(s, 0);
    uint32_t phigh = win32_arg(s, 1);
    struct stat st;
    if (fstat(fd, &st) == 0) {
        uint64_t sz = (uint64_t)st.st_size;
        if (phigh) win32_mem_write32(phigh, (uint32_t)(sz >> 32));
        s.gpr.rax.dword = (uint32_t)sz;
    } else {
        if (phigh) win32_mem_write32(phigh, 0);
        s.gpr.rax.dword = 0xFFFFFFFF;
    }
    win32_stdcall_ret(s, 2);
}

// GetFileTime(hFile, lpCreationTime, lpLastAccessTime, lpLastWriteTime) : FILETIME = centaines de nanosecondes
// depuis 1601. Jamais ecrite : Nuketown (zm_nuked) l'appelle sur l'exe depuis le fil serveur, le talon ne rendait
// pas ses 16 octets de pile (ret -> 0) et le jeu recommencait sans fin (978 576 appels, run play-zombies-20261009_102704).
static void impl_GetFileTime(State& s) {
    struct stat st;
    if (fstat((int)win32_arg(s, 0), &st) != 0) {
        t_last_error = 6u;   // ERROR_INVALID_HANDLE
        s.gpr.rax.dword = 0;
        win32_stdcall_ret(s, 4);
        return;
    }
    const timespec t[3] = {BO2_ST_BIRTHTIM(st), BO2_ST_ATIM(st), BO2_ST_MTIM(st)};
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t out = win32_arg(s, 1 + i);
        if (!out) continue;
        const uint64_t ft = (uint64_t(t[i].tv_sec) + 11644473600ull) * 10000000ull + uint64_t(t[i].tv_nsec) / 100;
        win32_mem_write32(out, uint32_t(ft));
        win32_mem_write32(out + 4, uint32_t(ft >> 32));
    }
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 4);
}

// SetFilePointer(handle, lDistanceToMove, lpDistanceToMoveHigh, method).
//
// 🔑 THE HIGH DWORD IS NOT OPTIONAL HERE. base.ipak is 2493 MB and its own header
// points at a data section ABOVE THE 2 GB LINE, so every seek into the image data
// carries a distance that does not fit in a signed 32-bit value. The old version
// ignored lpDistanceToMoveHigh entirely and cast the low word to int32_t, which
// turned an offset like 0x9BD40000 into -1680113664 and made lseek fail — so the
// engine could open the ipaks, read their headers, and never read a single image
// byte out of them.
//
// Win32 semantics: when lpDistanceToMoveHigh is non-null the two words form ONE
// signed 64-bit distance, and the high dword of the RESULT is written back through
// it. When it is null the distance is a signed 32-bit value.
static void impl_SetFilePointer(State& s) {
    uint32_t fd = win32_arg(s, 0);
    uint32_t low = win32_arg(s, 1);
    uint32_t phigh = win32_arg(s, 2);
    uint32_t method = win32_arg(s, 3);
    int whence = SEEK_SET;
    if (method == 1) whence = SEEK_CUR;
    if (method == 2) whence = SEEK_END;
    int64_t dist;
    if (phigh) {
        int32_t high = (int32_t)win32_mem_read32(phigh);
        dist = (int64_t)(((uint64_t)(uint32_t)high << 32) | (uint64_t)low);
    } else {
        dist = (int64_t)(int32_t)low;
    }
    off_t result = lseek(fd, (off_t)dist, whence);
    if (result < 0) {
        static uint32_t nfail = 0;
        if (++nfail <= 8)
            fprintf(stderr, "[SetFilePointer] FAILED fd=%u dist=%lld method=%u\n",
                    fd, (long long)dist, method);
        if (phigh) win32_mem_write32(phigh, 0xFFFFFFFFu);
        s.gpr.rax.dword = 0xFFFFFFFFu;          // INVALID_SET_FILE_POINTER
    } else {
        if (phigh) win32_mem_write32(phigh, (uint32_t)((uint64_t)result >> 32));
        s.gpr.rax.dword = (uint32_t)((uint64_t)result & 0xffffffffu);
    }
    win32_stdcall_ret(s, 4);
}

// --- SYNCHRONIZATION ---
// CRITICAL_SECTION emulation. A Win32 CRITICAL_SECTION lives in 32-bit *guest*
// memory, but a pthread_mutex_t* is a 64-bit host pointer — storing one in the
// guest CS slot truncates the high 32 bits (arm64 malloc returns addresses above
// the 4 GB line), so Enter later derefs garbage and segfaults. Instead we keep
// the guest-CS-address -> host-mutex mapping entirely host-side and never write a
// host pointer into guest memory. Win32 critical sections are re-entrant, so the
// backing mutex is PTHREAD_MUTEX_RECURSIVE.
// (Single-threaded during CRT init; guard this map if real threads are spawned.)
static std::unordered_map<uint32_t, pthread_mutex_t*> g_crit_sections;

static pthread_mutex_t* cs_get_or_create(uint32_t cs_ptr) {
    CsLock _cl;   // g_crit_sections est un unordered_map : une insertion concurrente le casse
    auto it = g_crit_sections.find(cs_ptr);
    if (it != g_crit_sections.end()) return it->second;
    pthread_mutex_t* m = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    g_crit_sections[cs_ptr] = m;
    return m;
}

static void impl_InitializeCriticalSection(State& s) {
    cs_get_or_create(win32_arg(s, 0));
    win32_stdcall_ret(s, 1);
}

static void impl_InitializeCriticalSectionAndSpinCount(State& s) {
    // (lpCriticalSection, dwSpinCount) — 2 args; spin count ignored.
    cs_get_or_create(win32_arg(s, 0));
    s.gpr.rax.dword = 1;  // BOOL success
    win32_stdcall_ret(s, 2);
}

// 🔴 DEUX DEFAUTS ICI, ET LE SECOND NE SE CORRIGE PAS AVEC UN VERROU DE TABLE.
//
//  (a) la table etait touchee sans verrou : un `erase` pendant qu'un fil hote
//      `find` casse le chainage de seau. Corrige par `CsLock`.
//  (b) `pthread_mutex_destroy` + `free` DETRUISENT un mutex sur lequel un fil
//      hote peut etre en train de bloquer. Aucun verrou de table n'empeche ca :
//      l'attente a lieu DANS le mutex, pas dans la table. Or notre portage
//      termine des fils que Windows ne terminerait pas, donc « personne ne le
//      tient » n'est pas garanti.
//      ⇒ quand une coroutine est hebergee, on RETIRE de la table sans liberer.
//      Cote invite la semantique est identique (apres Delete, un Enter recree
//      paresseusement une section neuve) ; seul le mutex HOTE survit, 64 octets
//      par suppression. Le chemin par defaut (rien d'heberge) garde le
//      destroy/free, bit pour bit.
static std::vector<pthread_mutex_t*> g_cs_retires;   // mutex retires, jamais liberes
static void impl_DeleteCriticalSection(State& s) {
    CsLock _cl;
    auto it = g_crit_sections.find(win32_arg(s, 0));
    if (it != g_crit_sections.end()) {
        if (__builtin_expect(g_any_hosted, 0)) {
            g_cs_retires.push_back(it->second);       // on ne libere pas : voir (b)
        } else {
            pthread_mutex_destroy(it->second);
            free(it->second);
        }
        g_crit_sections.erase(it);
    }
    win32_stdcall_ret(s, 1);
}

// ── 🎯 BO2_CS_WHO=1 — NAME THE LOCK LOOP ────────────────────────────────────
//
// [IMP-HIST] at the angola stall: 2,265,248 import calls since the last
// presented frame, of which 1,131,963 LeaveCriticalSection and 1,131,951
// EnterCriticalSection — a perfectly balanced pair and 100.0% of the traffic.
// The guest takes and releases a lock a million times and produces no frame.
//
// That is a NEW situation and a better one: this morning the same stall showed
// imports(+0), a loop inside one lifted block that no probe could see. Now the
// loop calls imports, so it can be named — by return address and by which
// critical section object it is hammering.
//
// Bounded to 16 (site, object) pairs and off by default; the counters are two
// adds on a hot path when armed, nothing when not.
static uint32_t g_cs_site[16], g_cs_obj[16];
static uint64_t g_cs_n[16];
static int      g_cs_used;
extern "C" void win32_cs_report(void) {
    if (!bo2_getenv("BO2_CS_WHO")) return;
    static uint64_t prev[16];
    for (int i = 0; i < g_cs_used; i++) {
        const uint64_t d = g_cs_n[i] - prev[i];
        prev[i] = g_cs_n[i];
        if (d) fprintf(stderr, "[CS-WHO] EnterCriticalSection(0x%08x) from 0x%08x — %llu call(s) "
                       "since the last report (total %llu)%s\n", g_cs_obj[i], g_cs_site[i],
                       (unsigned long long)d, (unsigned long long)g_cs_n[i],
                       d > 100000 ? "   <- THIS is the loop" : "");
    }
}
// ── 🎯 PER-COROUTINE CRITICAL-SECTION DEPTH ─────────────────────────────────
//
// WHY THIS EXISTS, AND WHY IT IS NOT OPTIONAL. Guest critical sections are
// PTHREAD_MUTEX_RECURSIVE, and a recursive mutex identifies its owner by the HOST
// THREAD. Every guest coroutine shares one host thread. So if coroutine A yields
// while holding L, B's lock of L is not a rare race — the mutex counts it as a
// LEGITIMATE RECURSIVE RE-ENTRY, returns success, and B runs inside the critical
// section beside A. On release B decrements to 1 and A to 0, and nothing reports
// an error. It works silently, which is worse than crashing.
//
// That makes it certain on every yield-under-lock, not probabilistic — so any
// preemption experiment is uninterpretable until the depth is known.
//
// THE NUMBER THIS ANSWERS: at the freeze, is the wedged coroutine holding a lock?
//   depth 0  -> a "yield only at depth 0" guard preserves mutual exclusion
//               exactly, and the preemption A/B becomes interpretable.
//   depth > 0 -> that guard would never fire on the wedged coroutine and would
//               measure a no-op; the real repair is coroutine-AWARE guest
//               critical sections (own mutex keyed by coroutine id, block = yield)
//               rather than leaning on pthread at all.

// ── 🎯 [CS-SHARED] — IS MUTUAL EXCLUSION BETWEEN GUEST THREADS REAL? ─────────
//
// Every guest thread is a coroutine on ONE host thread, and a guest
// CRITICAL_SECTION is a PTHREAD_MUTEX_RECURSIVE host mutex. So when coroutine A
// holds a section and yields (a WaitForSingleObject, a Sleep), coroutine B's
// EnterCriticalSection on the same section SUCCEEDS at once — same host thread,
// recursive mutex. On Windows B would block until A leaves. If this ever
// happens, the engine's lock-protected structures (the script variable pools,
// the MT allocator — sub_4a1ee0/0x586800 are its _lock/_unlock) are modified
// by two threads at once, which is exactly the family of corruption the angola
// freeze is made of. Track the owning coroutine and depth per section, and
// report every entry into a section another coroutine holds.
struct CsOwn { int owner = -1; uint32_t depth = 0, owner_ret = 0; };
static std::unordered_map<uint32_t, CsOwn> g_cs_own;
static uint64_t g_cs_shared_n, g_cs_waits, g_cs_wait_turns;
// The last 64 lock operations, for reading a deadlock's history rather than its
// final state: op E=enter (after waiting, if any), L=leave, T=try-enter (r = result).
struct CsOp { char op; int8_t tid; uint8_t r; uint32_t cs, ret; uint64_t seq; };
static CsOp g_csop[64]; static uint64_t g_csop_n;
static inline void csop_note(char op, uint32_t cs, uint32_t ret, int r) {
    CsOp& o = g_csop[g_csop_n & 63u]; o.op = op; o.tid = (int8_t)g_cur; o.r = (uint8_t)r; o.cs = cs; o.ret = ret; o.seq = ++g_csop_n;
}
static void evop_report();
// 🎯 LE MOTEUR ADRESSE SES SECTIONS PAR INDEX, PAS PAR ADRESSE. Les trois
// enveloppes du jeu calculent toutes `0x02b535e8 + idx*24` :
//     mov eax,[esp+4] ; lea eax,[eax+eax*2] ; lea ecx,[eax*8+0x2b535e8]
// (Enter 0x004a1ee0, Leave 0x00586800, TryEnter 0x005f7d60). Imprimer l'adresse
// seule obligeait a refaire cette division a la main pour retrouver le `push N`
// du site appelant — c'est ce qui a coute une session entiere sur runs/p563.
static const char* cs_label(uint32_t cs, char* buf, size_t n) {
    const uint32_t base = 0x02b535e8u;
    if (cs >= base && ((cs - base) % 24u) == 0u && (cs - base) / 24u < 128u)
        snprintf(buf, n, "0x%08x (section #%u)", cs, (cs - base) / 24u);
    else
        snprintf(buf, n, "0x%08x (hors du tableau du moteur)", cs);
    return buf;
}
// Le retour d'un appel d'import identifie l'enveloppe, donc COMMENT la section a
// ete prise : « taken at 0x005f7d75 » veut dire TryEnter, pas Enter.
static const char* cs_how(uint32_t ret) {
    switch (ret) {
        case 0x004a1ef5u: return " par Enter";
        case 0x00586815u: return " par Leave";
        case 0x005f7d75u: return " par TryEnter";
        default:          return "";
    }
}
// 🔴 DEFAUT REMIS A **OFF** LE 2026-09-20, PAR MESURE, CONTRE MON PROPRE CORRECTIF.
//
// `934be48` avait arme cette cession pour lever un VERROU VIVANT au chargement
// d'angola (« 1 run sur 5 »). Elle le leve. Mais elle produit un mal pire, et
// de facon deterministe :
//
//   | run     | cession | soudure liste libre | arret d'affichage |
//   |---------|---------|---------------------|-------------------|
//   | sched1  | non     | non                 | 634 s             |
//   | proxy1  | non     | non                 | jamais            |
//   | trycs1  | OUI     | OUI                 | 100 s             |
//   | trycs2  | OUI     | OUI                 | 110 s             |
//   | excl1   | OUI     | OUI                 | 110 s             |
//   | soud1   | OUI     | OUI (41 097 fois)   | 124 s             |
//
// 3/3 puis 4/4 avec, 0/2 sans. L'echange est donc : une chance sur cinq de
// geler au CHARGEMENT contre la CERTITUDE de perdre l'affichage apres deux
// minutes de jeu. Pour quelqu'un qui veut jouer le niveau, OFF gagne d'un
// facteur cinq a six sur la duree de jeu.
//
// ⚠️ CE N'EST PAS UN CORRECTIF ET CE N'EST PAS UNE GARDE. Le defaut reel est
// sous la cession : `sub_578b10` reinsere en liste libre un enregistrement qui
// EST deja la tete (`[VARFREE-SOUDURE]` le prend en flagrant delit, premiere
// occurrence avec `tag=0x17` = deja libre, appelant `0x00648591`). La cession
// ne fait que rendre l'entrelacement qui le declenche systematique. Tant que
// ce defaut-la n'est pas nomme, on ne rearme pas ce defaut-ci.
//
// `BO2_TRYCS_YIELD=1` reste le bras d'essai : c'est aujourd'hui le seul moyen
// connu de reproduire la soudure a la demande, en moins de deux minutes.
static inline bool trycs_yield_on() {
    static const bool on = [] {
        const char* e = bo2_getenv("BO2_TRYCS_YIELD"); return e && atoi(e) != 0;
    }();
    return on;
}

// 🔴 L'EXPOSITION DOIT ETRE CHIFFRABLE, SINON L'A/B NE VEUT RIEN DIRE.
// Le temoin [TRYCS] imprime aux puissances de deux : sur runs/trycs2 il a laisse
// 13 LIGNES, que j'ai lues « 13 cessions » alors que la derniere disait #512 —
// donc entre 512 et 1023. Un compteur ecrete lu comme un total, pour la
// quatrieme fois dans ce depot. Ces trois totaux donnent la variable
// d'exposition des deux bras : avec BO2_TRYCS_YIELD=0 on voit les ECHECS sans
// les CESSIONS, ce qui est exactement le contraste qu'on veut mesurer.
static uint64_t g_trycs_calls = 0, g_trycs_fail = 0, g_trycs_yield = 0,
                g_trycs_refus_cb = 0, g_trycs_refus_lock = 0;

static void cs_lock_report() {
    evop_report();
    fprintf(stderr, "[TRYCS] total : %llu TryEnter, dont %llu refuses, dont %llu "
                    "cessions effectives (BO2_TRYCS_YIELD=%s) ; cessions REFUSEES : "
                    "%llu en rappel de shim, %llu sous verrou sans CS_COOP\n",
            (unsigned long long)g_trycs_calls, (unsigned long long)g_trycs_fail,
            (unsigned long long)g_trycs_yield, trycs_yield_on() ? "1" : "0",
            (unsigned long long)g_trycs_refus_cb, (unsigned long long)g_trycs_refus_lock);
    {   const uint64_t n = g_csop_n, from = n > 64 ? n - 64 : 0;
        fprintf(stderr, "[LOCKS] last %llu lock operations (op tid section caller result):\n", (unsigned long long)(n - from));
        for (uint64_t k = from; k < n; k++) { const CsOp& o = g_csop[k & 63u];
            char lb[64];
            fprintf(stderr, "[LOCKS]   #%llu %c tid %d %s from 0x%08x%s\n", (unsigned long long)o.seq, o.op, o.tid,
                    cs_label(o.cs, lb, sizeof lb), o.ret,
                    o.op == 'T' ? (o.r ? " -> got it" : " -> FAILED (held by another)") : ""); }
    }
    // Lock state, for the deadlocks a correct critical section can now produce:
    // who is waiting for which section, and who holds every held section.
    // Une seule prise pour les DEUX parcours ci-dessous. Le dump n'a pas de
    // cession, donc la regle de tenue est respectee.
    CsLock _cl;
    for (int t = 0; t < 64 && (size_t)t < sched_nthreads(); t++)
        if (g_cs_waiting_for[t]) {
            auto it = g_cs_own.find(g_cs_waiting_for[t]);
            char lb[64];
            const uint32_t oret = it != g_cs_own.end() ? it->second.owner_ret : 0u;
            fprintf(stderr, "[LOCKS] tid %d WAITS for section %s, held by tid %d (depth %u, taken at 0x%08x%s)\n",
                    t, cs_label(g_cs_waiting_for[t], lb, sizeof lb), it != g_cs_own.end() ? it->second.owner : -1,
                    it != g_cs_own.end() ? it->second.depth : 0u, oret, cs_how(oret));
        }
    for (auto& kv : g_cs_own)
        if (kv.second.depth)
        {   char lb[64];
            fprintf(stderr, "[LOCKS] section %s HELD by tid %d, depth %u, taken at 0x%08x%s\n",
                    cs_label(kv.first, lb, sizeof lb), kv.second.owner, kv.second.depth,
                    kv.second.owner_ret, cs_how(kv.second.owner_ret));
        }
}

static void cs_owner_enter(uint32_t cs, uint32_t ret) {
    CsLock _cl;   // g_cs_own idem — et la reference `o` ne survit pas a la portee
    CsOwn& o = g_cs_own[cs];
    if (o.depth && o.owner != g_cur) {
        ++g_cs_shared_n;
        if (g_cs_shared_n <= 12 || (g_cs_shared_n & (g_cs_shared_n - 1)) == 0)
            fprintf(stderr, "🎯 [CS-SHARED] #%llu tid %d ENTERED critical section 0x%08x (from 0x%08x) while tid %d holds it "
                            "(depth %u, taken at 0x%08x) — on Windows this thread would block here\n",
                    (unsigned long long)g_cs_shared_n, (int)g_cur, cs, ret, o.owner, o.depth, o.owner_ret);
        // Keep the section's accounting coherent: the newcomer now "shares" it.
        o.owner = g_cur; o.owner_ret = ret; ++o.depth;
        return;
    }
    if (!o.depth) { o.owner = g_cur; o.owner_ret = ret; }
    ++o.depth;
}
static void cs_owner_leave(uint32_t cs) {
    CsLock _cl;
    auto it = g_cs_own.find(cs);
    if (it == g_cs_own.end() || !it->second.depth) return;
    if (--it->second.depth == 0) it->second.owner = -1;
}
// ── 🎯 CE QUI A TUE runs/snd1, ET POURQUOI CE N'EST PAS UNE RUSTINE ─────
//
// Avant l'hebergement, `pthread_mutex_lock` sur une section critique invitee ne
// bloquait JAMAIS : toutes les coroutines partagent un fil hote et le mutex est
// recursif. Des qu'une coroutine tourne sur son PROPRE fil, ce mutex bloque pour
// de vrai — et s'il bloque le FIL ORDONNANCEUR, il ne bloque pas une coroutine,
// **il les bloque toutes les douze d'un coup**. Le jeu entier s'arrete sur une
// section que Windows aurait fait attendre a un seul thread.
//
// Mesure : runs/snd1, section #40 (le verrou de physicalmemory.cpp) tenue par
// tid 4 a depth 2, 14 [DEADLOCK-DUMP], et le niveau n'a JAMAIS affiche une seule
// image (maxDraws=5 contre 4446 et 4238 sur les deux runs de reference, qui
// atteignent le niveau en 22-26 s).
//
// ⇒ Sur le fil ordonnanceur on ne bloque pas : on **cede la coroutine** jusqu'a
//    ce que le fil hote lache. C'est la semantique Win32 (le thread qui attend
//    attend, les autres avancent), pas une garde : sans ca, nos douze threads
//    invites n'en font qu'un seul aux yeux du verrou.
//    Sur un fil hote, `pthread_mutex_lock` reste correct : il ne bloque que lui.
static void cs_lock_without_stalling_the_scheduler(uint32_t cs) {
    pthread_mutex_t* m = cs_get_or_create(cs);
    if (__builtin_expect(!g_any_hosted, 1) || (k32_hors_ordonnanceur() && t_host_tid >= 0)) {
        pthread_mutex_lock(m);              // chemin d'aujourd'hui, inchange
        return;
    }
    // Fil ordonnanceur, et au moins une coroutine est hebergee.
    //
    // 🔴 LE TEMOIN EST ARME A L'ENTREE DE L'ATTENTE, JAMAIS A SA SORTIE.
    // La version d'origine n'imprimait qu'APRES la boucle. Or le risque que
    // cette fonction existe pour surveiller — un fil hote qui garde la section
    // et fige les douze coroutines — est precisement une attente qui **ne sort
    // jamais** : le `fprintf` place apres ne s'execute alors pas, et un run gele
    // rend `[CS-HOTE] = 0`, qui se lit « aucune contention » alors qu'il veut
    // dire « contention infinie ». C'est la troisieme fois dans ce fichier que
    // le seul temoin vit du mauvais cote d'une boucle d'attente (voir le meme
    // correctif deja applique a `[TRYCS]` plus bas) — et ca avait deja rendu
    // `[CS-HOTE]=0` ininterpretable sur `runs/snd2`.
    static uint64_t waits = 0, total = 0;   // fil ordonnanceur seul : pas de course
    uint64_t turns = 0;
    while (pthread_mutex_trylock(m) != 0) {
        if (turns == 0) {
            ++waits;
            if (waits <= 6 || (waits & (waits - 1)) == 0)
                fprintf(stderr, "[CS-HOTE] tid %d ATTEND la section 0x%08x tenue par "
                                "un fil hote (attente #%llu) — si aucune ligne « a cede » "
                                "ne suit pour cette attente, elle n'est jamais sortie\n",
                        (int)g_cur, cs, (unsigned long long)waits);
        }
        ++turns;
        coop_yield_from("CS-hote");
    }
    if (turns) {
        total += turns;
        if (waits <= 6 || (waits & (waits - 1)) == 0)
            fprintf(stderr, "[CS-HOTE] tid %d a cede %llu tours pour la section 0x%08x "
                            "tenue par un fil hote (attente #%llu, %llu tours en tout)\n",
                    (int)g_cur, (unsigned long long)turns, cs,
                    (unsigned long long)waits, (unsigned long long)total);
    }
}

// 🎯 C21 (2026-09-26) — PRENDRE UNE SECTION CRITIQUE INVITEE DEPUIS LE HARNAIS, SANS BLOQUER.
// Pour qu'un lecteur hote (le predicat d'injection SAFE=2) voie l'etat que le
// moteur publie sous ses verrous, et pas un etat intermediaire. Essai seulement :
// l'appelant vit sur le fil ordonnanceur, et y bloquer figerait toutes les
// coroutines. Echec si la section est tenue par une AUTRE coroutine (table des
// proprietaires : le mutex recursif ne l'exclut pas, meme fil hote) ou par un fil
// heberge (le mutex). Chaque succes se rend par bo2_guest_cs_leave.
extern "C" bool bo2_guest_cs_try(uint32_t cs) {
    {   CsLock _cl;
        auto it = g_cs_own.find(cs);
        if (it != g_cs_own.end() && it->second.depth && it->second.owner != g_cur) return false;
    }
    if (pthread_mutex_trylock(cs_get_or_create(cs)) != 0) return false;
    cs_owner_enter(cs, 0u);
    return true;
}
extern "C" void bo2_guest_cs_leave(uint32_t cs) {
    cs_owner_leave(cs);
    pthread_mutex_unlock(cs_get_or_create(cs));
}

static void impl_EnterCriticalSection(State& s) {
    static const bool who = bo2_getenv("BO2_CS_WHO") != nullptr;
    if (__builtin_expect(who, 0)) {
        const uint32_t obj = win32_arg(s, 0);
        uint32_t ret = win32_mem_read32(s.gpr.rsp.dword);
        // 🎯 ONE FRAME FURTHER WHEN THE CALLER IS THE LOCK HELPER. sub_4a1ee0 is
        // five instructions, decoded from the file:
        //     mov eax,[esp+4] ; lea ; lea ecx,[eax*8+0x2b535e8] ; push ecx ; call
        // so at THIS entry the stack is ret(0x4a1ef5), ecx, the helper's own
        // return address, the lock id — and [esp+8] is the engine site that
        // actually wanted the lock. Gated on that exact return address, because
        // transposing a frame layout between call sites is a mistake this
        // project has already paid for.
        if (ret == 0x004a1ef5u) ret = win32_mem_read32(s.gpr.rsp.dword + 8u);
        int i = 0;
        for (; i < g_cs_used; i++) if (g_cs_site[i] == ret && g_cs_obj[i] == obj) break;
        if (i == g_cs_used && g_cs_used < 16) {
            g_cs_site[g_cs_used] = ret; g_cs_obj[g_cs_used] = obj; g_cs_used++;
        }
        if (i < 16) ++g_cs_n[i];
    }
    // Lazy create-on-first-use: an un-Initialize'd (e.g. static) CS still works.
    // 🎯 BO2_CS_COOP=1 — A CRITICAL SECTION HELD BY ANOTHER GUEST THREAD BLOCKS.
    // The host mutex below is recursive and every guest thread is a coroutine of
    // the same host thread, so on its own it excludes nothing: [CS-SHARED]
    // measured 21 entries per angola run into a section another guest thread
    // held (tid 2 into 0x02b53bb8 while tid 3 had it). Windows would make the
    // newcomer wait; so do that — yield until the holder leaves. This is the Win32
    // semantics, not a guard. A holder that is DEAD cannot leave (our port ends
    // threads Windows would not), so its section is reclaimed, loudly.
    {
        if (cs_coop_on()) {
            const uint32_t cs = win32_arg(s, 0);
            uint64_t turns = 0;
            int tenue_par = -1;
            const bool repos = !hosted_self() && ordo_repos_niveau() >= 3 && g_cur >= 0 && g_cur < 64;   // 14z
            for (;;) {
                // ⚠️ La table est lue sous `CsLock`, mais la CESSION se fait hors
                // portee : tenir ce verrou au travers d'un `coop_yield_from` le
                // donnerait au FIL hote pendant qu'une autre coroutine du meme
                // fil tourne. On prend donc un instantane, puis on le lache.
                int ow = -1; bool libre = true;
                {   CsLock _cl;
                    auto it = g_cs_own.find(cs);
                    if (it != g_cs_own.end() && it->second.depth && it->second.owner != g_cur) {
                        libre = false; ow = it->second.owner;
                    }
                }
                if (libre) break;
                if (ow < 0 || (size_t)ow >= sched_nthreads() || !g_threads[ow] || g_threads[ow]->status == T_DONE) {
                    static int ab = 0;
                    if (ab++ < 8)
                        fprintf(stderr, "[CS-COOP] section 0x%08x was held by tid %d, which is gone — reclaimed for tid %d\n", cs, ow, (int)g_cur);
                    {   CsLock _cl;
                        auto it = g_cs_own.find(cs);
                        if (it != g_cs_own.end()) { it->second.depth = 0; it->second.owner = -1; }
                    }
                    break;
                }
                tenue_par = ow;
                if (g_cur >= 0 && g_cur < 64) g_cs_waiting_for[g_cur] = cs;
                if (repos) {
                    if (!turns) g_ordo_cs_attentes.fetch_add(1);
                    ordo_repos_si_inactif(g_threads[g_cur], ~0ull, "CS-wait");
                }
                ++turns;
                coop_yield_from("CS-wait");
            }
            if (g_cur >= 0 && g_cur < 64) g_cs_waiting_for[g_cur] = 0;
            if (turns) {
                if (repos) g_ordo_cs_attentes.fetch_sub(1);
                ++g_cs_waits; g_cs_wait_turns += turns;
                if (g_cs_waits <= 6 || (g_cs_waits & (g_cs_waits - 1)) == 0)
                    fprintf(stderr, "[CS-COOP] tid %d waited %llu scheduler turns for section 0x%08x held by tid %d (wait #%llu, %llu turns in all)\n",
                            (int)g_cur, (unsigned long long)turns, cs, tenue_par, (unsigned long long)g_cs_waits, (unsigned long long)g_cs_wait_turns);
            }
        }
    }
    cs_lock_without_stalling_the_scheduler(win32_arg(s, 0));
    if (g_cur >= 0 && g_cur < 64) ++g_cs_depth[g_cur];
    cs_owner_enter(win32_arg(s, 0), win32_mem_read32(s.gpr.rsp.dword));
    csop_note('E', win32_arg(s, 0), win32_mem_read32(s.gpr.rsp.dword), 1);
    win32_stdcall_ret(s, 1);
}

static void impl_LeaveCriticalSection(State& s) {
    csop_note('L', win32_arg(s, 0), win32_mem_read32(s.gpr.rsp.dword), 1);
    cs_owner_leave(win32_arg(s, 0));
    if (g_ordo_cs_attentes.load() > 0 && hosted_self()) {   // 14z : la section qu'une coroutine du tourniquet attend
        for (int t = 0, n = (int)std::min<size_t>(sched_nthreads(), 64); t < n; t++)
            if (g_cs_waiting_for[t] == win32_arg(s, 0) && !g_threads[t]->hosted) {
                dispatch_semaphore_signal(ordo_sem());
                break;
            }
    }
    // Le verrou ne couvre que la LECTURE de la table ; `pthread_mutex_unlock`
    // se fait hors portee (il ne bloque pas, mais la regle de tenue ci-dessus
    // dit qu'on ne tient jamais ce verrou au travers d'une primitive de
    // section invitee — on ne fait pas d'exception qu'il faudrait relire).
    pthread_mutex_t* m = nullptr;
    {   CsLock _cl;
        auto it = g_crit_sections.find(win32_arg(s, 0));
        if (it != g_crit_sections.end()) m = it->second;
    }
    if (m) pthread_mutex_unlock(m);
    // ⚠️ Clamp at zero rather than trusting pairing: an unmatched Leave (or one
    // whose Enter happened before this counter existed) must not drive the depth
    // negative and make a held lock read as free.
    if (g_cur >= 0 && g_cur < 64 && g_cs_depth[g_cur] > 0) --g_cs_depth[g_cur];
    win32_stdcall_ret(s, 1);
}

// 🎯 UN TryEnter QUI ECHOUE DOIT CEDER, SINON LE PROPRIETAIRE NE PEUT PAS AVANCER.
// Mesure runs/snd2 : tid 5 s'est vu refuser la section #40 (0x02b539a8, le verrou
// de physicalmemory.cpp) 8 341 960 fois depuis 0x005f7d75 -- un emballage invite
// `BOOL f(cs){ return !!TryEnterCriticalSection(cs); }` appele en boucle par la
// file de travaux -- jusqu'a bruler les 400 M d'imports du garde-fou et abattre le
// processus (signal 6, "run ends at #400000001"). Pendant tout ce temps
// `[SCHED] coeur=0ms` : AUCUNE commutation. Le cycle mesure :
//
//     tid 5 spin sur #40, ne cede jamais  =>  tid 3 ne tourne jamais
//     tid 3 tient #67 et dort             =>  tid 0 n'obtient jamais #67
//     tid 0 tient #40 et attend #67       =>  #40 n'est jamais relachee
//
// Ce n'est pas un interblocage, c'est un VERROU VIVANT, et c'est le gel "1 run
// sur 5" pendant le chargement d'angola, enfin nomme mecaniquement.
// Sur Windows ce spin est SAIN : le proprietaire avance sur un AUTRE coeur. Ici
// il n'y a qu'un fil hote et notre ordonnanceur ne preempte jamais, donc le spin
// empeche exactement ce qu'il attend. Meme panne que le sondage WFSO(h,0) de
// SV_Frame, une primitive plus loin.
//
// ⚠️ CE N'EST PAS UNE GARDE : ceder n'est pas bloquer. La coroutine reprend a
// la meme frontiere d'instruction, exactement la ou Windows l'aurait preemptee.
// On restitue la garantie de progres que le jeu suppose, on ne masque rien.
// L'etat invite est COMPLET au moment de la cession (valeur de retour posee, pile
// depilee par win32_stdcall_ret) : une autre coroutine ne peut pas voir un
// demi-appel.
// 🔴 INTERRUPTEUR AJOUTE LE 2026-09-20, ET PAS POUR L'HYGIENE D'A/B : POUR
// POUVOIR DESARMER UNE REGRESSION MESUREE.
// La cession corrige bien le verrou vivant de runs/snd2, mais elle a un cout
// mesure 2/2 contre 2/2 (P585) : avec elle, la liste libre du pool de variables
// GSC SERVEUR se soude sur elle-meme (longueur 1 pour 24 228 enregistrements
// marques libres) et la boucle de drain de 0x495730 tourne 96 millions de fois
// sur le meme enregistrement, ecran noir a 105 s. Sans elle : liste saine,
// ~500 tours.
//
//     run      liste libre serveur      appels [E880-AMONT]   arret
//     sched1   18805, saine                            512    634 s
//     proxy1   15724, saine                            408    jamais
//     trycs1   1, AUTO-CYCLE                    90 355 876    100 s
//     trycs2   1, AUTO-CYCLE                    96 489 510    110 s
//
// Le defaut reste 1 : la cession est juste sur le fond, et surtout elle donne ce
// qui manquait depuis des semaines -- un REPRODUCTEUR FIABLE de la soudure
// (2/2 en moins de 110 s, contre « 1 run sur 5 »). On garde le banc d'essai
// arme ; `BO2_TRYCS_YIELD=0` rend l'ancien comportement sur le MEME binaire.
static void cs_try_failed_so_let_the_owner_run(uint32_t cs) {
    ++g_trycs_fail;
    // 🔴 P628/E7 — UN FIL HOTE NE PEUT PAS « laisser tourner le detenteur » : il
    // n'a pas de tourniquet. Il rend 0 a l'invite, qui reboucle aussitot, donc il
    // martele le verrou a pleine vitesse de cœur. Mesure : 24 a 61 M de refus
    // contre 13-19 k au temoin, 3/3 contre 0/3 (P615, memoire
    // bo2-physmem-lock-deadlock, g_cs = 0x2b535e8). Pendant ce temps le
    // detenteur est une coroutine qui attend son tour sur un fil ordonnanceur
    // qui n'a plus de cœur libre : c'est un verrou VIVANT que nous fabriquons.
    //
    // ⚠️ CE N'EST PAS `BO2_TRYCS_YIELD`, et la distinction est tout l'argument.
    //    Ce drapeau-la CEDE : il fait tourner une AUTRE coroutine, eventuellement
    //    sur un `State` emprunte par un rappel de shim — 6 double-liberations
    //    d'allocateur mesurees, ce qui est exactement ce que les exclusions
    //    ci-dessous corrigent. Ici on DORT. Personne ne tourne, aucun `State`
    //    n'est emprunte, aucun `swapcontext`. L'objection qui tue l'un n'atteint
    //    pas l'autre ; les deux gestes ne partagent que le mot « ceder ».
    //
    // Fidelite (regle 7) : sur Windows le fil qui echoue son TryEnter ET le
    // detenteur tournent chacun sur leur cœur, donc la boucle de reessai se
    // resout en nanosecondes. La reculade RESTAURE cette relation temporelle ;
    // elle ne masque aucun defaut du jeu.
    //
    // Pas de compteur nouveau : `g_trycs_fail` est incremente juste au-dessus et
    // mesure deja la metrique de P628.8. Un second global chaud serait justement
    // la contention qu'on corrige.
    if (__builtin_expect(g_any_hosted, 0) && k32_hors_ordonnanceur() && t_host_tid >= 0) {
        host_backoff(t_host_tid);
        return;
    }
    if (!trycs_yield_on()) return;
    // 🔴 EXCLUSIONS IDENTIQUES A BO2_PREEMPT_W ET AU SONDAGE WFSO(h,0) — ET MON
    // PREMIER CORRECTIF LES AVAIT TOUTES LES DEUX OUBLIEES.
    // C'etait le SEUL site de cession du runtime a les ignorer : les quatre
    // autres (win32_import_tick, win32_thread_write_tick, la cession de
    // SetEvent, et le sondage WFSO dont mon propre commentaire se reclamait
    // explicitement) testent `g_in_shim_callback` avant de ceder.
    //
    // Pourquoi ca compte ici precisement : un rappel de shim (la rampe WNDPROC,
    // la pompe OnBufferEnd de XAudio2) EMPRUNTE le State de la coroutine
    // courante. Ceder dedans laisse une autre coroutine tourner sur un etat
    // emprunte. Or le temoin [TRYCS] accuse **tid 5**, que [SCHED] nomme
    // `@audio-throttle` : le rappel audio est exactement ce cas. Et la mesure
    // 2/2 contre 2/2 dit que la cession produit 6 double-liberations
    // d'allocateur la ou le bras temoin en a zero.
    //
    // ⚠️ Ce n'est pas une garde qui masque un defaut du jeu : c'est l'invariant
    // que ce runtime s'est deja donne, applique a un site qui l'avait oublie.
    if (in_shim_cb()) { ++g_trycs_refus_cb; return; }
    const bool ulock = (g_cur >= 0 && g_cur < 64 && g_cs_depth[g_cur] > 0);
    if (ulock && !cs_coop_on()) { ++g_trycs_refus_lock; return; }
    ++g_trycs_yield;
    const uint64_t fails = g_trycs_yield;
    // Temoin arme a l'ENTREE de la cession, jamais a sa sortie : un temoin place
    // APRES une attente est muet quand l'attente est infinie -- c'est ce qui a
    // rendu [CS-HOTE]=0 ininterpretable sur runs/snd2, et c'est la deuxieme fois
    // dans la journee que le seul fprintf vivait du mauvais cote de la boucle.
    if (fails <= 6 || (fails & (fails - 1)) == 0)
        fprintf(stderr, "[TRYCS] tid %d : TryEnter refuse sur la section 0x%08x — "
                        "je cede un tour pour que le proprietaire avance (echec #%llu)\n",
                (int)g_cur, cs, (unsigned long long)fails);
    coop_yield_from("TryCS");
}

static void impl_TryEnterCriticalSection(State& s) {
    ++g_trycs_calls;
    {   // Same semantics as Enter under BO2_CS_COOP: held by ANOTHER guest thread = fail.
        bool tenue_par_un_autre = false;
        {   CsLock _cl;
            auto it = g_cs_own.find(win32_arg(s, 0));
            tenue_par_un_autre = (it != g_cs_own.end() && it->second.depth
                                  && it->second.owner != g_cur);
        }
        if (cs_coop_on() && tenue_par_un_autre) {
            const uint32_t cs = win32_arg(s, 0);
            csop_note('T', cs, win32_mem_read32(s.gpr.rsp.dword), 0);
            s.gpr.rax.dword = 0;
            win32_stdcall_ret(s, 1);
            cs_try_failed_so_let_the_owner_run(cs);
            return;
        }
    }
    int ok = (pthread_mutex_trylock(cs_get_or_create(win32_arg(s, 0))) == 0);
    // 🎯 UN TryEnter QUI REUSSIT EST UN Enter. Il manquait le ++, alors que Leave
    // decremente : une section prise par TryEnter etait INVISIBLE a g_cs_depth,
    // donc au refus de preemption « under_lock » et au comptage g_imp_under_lock.
    // Sans BO2_CS_COOP le mutex hote est recursif et partage par toutes les
    // coroutines : ceder sous un verrou invisible laisse une autre coroutine
    // entrer dans la meme section. Le jeu emprunte reellement ce chemin —
    // runs/p563 : « #2532787 T tid 5 0x02b539a8 from 0x005f7d75 -> got it ».
    if (ok) {
        if (g_cur >= 0 && g_cur < 64) ++g_cs_depth[g_cur];
        cs_owner_enter(win32_arg(s, 0), win32_mem_read32(s.gpr.rsp.dword));
    }
    const uint32_t cs = win32_arg(s, 0);
    csop_note('T', cs, win32_mem_read32(s.gpr.rsp.dword), ok);
    s.gpr.rax.dword = ok;
    win32_stdcall_ret(s, 1);
    // Meme raison sur le chemin sans BO2_CS_COOP : le mutex hote a refuse, donc
    // quelqu'un d'autre le tient, donc il faut lui laisser un tour.
    if (!ok) cs_try_failed_so_let_the_owner_run(cs);
}

// --- ERROR HANDLING ---
static void impl_GetLastError(State& s) {
    static int n = 0;
    if (++n == 6000) {   // deep in the busy-loop: dump the call chain
        fprintf(stderr, "[loop-stack] ret=0x%08x ebp=0x%08x\n", win32_mem_read32(s.gpr.rsp.dword), s.gpr.rbp.dword);
        uint32_t bp = s.gpr.rbp.dword;
        for (int i = 0; i < 14 && bp >= 0x1000 && bp < 0xFFFFF000; i++) {
            fprintf(stderr, "  [chain] bp=0x%08x ret=0x%08x\n", bp, win32_mem_read32(bp + 4));
            uint32_t nb = win32_mem_read32(bp); if (nb <= bp) break; bp = nb;
        }
    }
    s.gpr.rax.dword = t_last_error;
    win32_stdcall_ret(s, 0);
}

static void impl_SetLastError(State& s) {
    static DiagCounter<12> s_sle;   // P660/C14
    uint32_t ret = win32_mem_read32(s.gpr.rsp.dword);
    if (s_sle.bump(ret) == 0) fprintf(stderr, "[SetLastError] arg=%u caller-ret=0x%08x\n", win32_arg(s,0), ret);
    t_last_error = win32_arg(s, 0);
    win32_stdcall_ret(s, 1);
}

// --- STRING ---
static void impl_lstrlenA(State& s) {
    uint32_t ptr = win32_arg(s, 0);
    uint32_t len = 0;
    while (win32_mem_read8(ptr + len)) len++;
    s.gpr.rax.dword = len;
    win32_stdcall_ret(s, 1);
}

static void impl_lstrlenW(State& s) {
    uint32_t ptr = win32_arg(s, 0);
    uint32_t len = 0;
    while (win32_mem_read8(ptr + len * 2) || win32_mem_read8(ptr + len * 2 + 1)) len++;
    s.gpr.rax.dword = len;
    win32_stdcall_ret(s, 1);
}

// --- MISC ---
static void impl_IsDebuggerPresent(State& s) {
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 0);
}

// A reserved guest region (above the PE image ~0x04189000, below TEB 0x7FFD0000)
// for shim-owned buffers the guest must be able to read via g_mem. Returning a
// host static pointer here would truncate on a 64-bit host and the guest would
// read zeros (host-pointer audit item #4).
static constexpr uint32_t GUEST_SCRATCH = 0x0F000000;
static uint32_t guest_write_cstr(uint32_t addr, const char* s) {
    uint32_t i = 0;
    for (; s[i]; i++) win32_mem_write8(addr + i, (uint8_t)s[i]);
    win32_mem_write8(addr + i, 0);
    return addr;
}
// BO2_CMDLINE_EXTRA : ajoute apres le nom de l'exe (des `+set dvar valeur`, que le moteur applique AVANT ses
// sous-systemes, donc avant l'init du son). Android le pose a `+set snd_speakerConfiguration Stereo` : le mixeur du
// jeu mixait en 5.1 (6 canaux, reverberation comprise) pour deux haut-parleurs.
static void impl_GetCommandLineA(State& s) {
    static uint32_t p = 0;
    if (!p) {
        std::string cl = "t6sp.exe";
        if (const char* x = bo2_getenv("BO2_CMDLINE_EXTRA"); x && *x && strlen(x) < 1024) { cl += ' '; cl += x; }
        p = guest_write_cstr(GUEST_SCRATCH, cl.c_str());
        fprintf(stderr, "[CMDLINE] %s\n", cl.c_str());
    }
    s.gpr.rax.dword = p;
    win32_stdcall_ret(s, 0);
}

extern "C" void bo2_dump_child_pools(const char* why);   // win32_runtime.cpp
static void impl_OutputDebugStringA(State& s) {
    uint32_t ptr = win32_arg(s, 0);
    char buf[1024];
    for (int i = 0; i < 1023; i++) {
        buf[i] = (char)win32_mem_read8(ptr + i);
        if (buf[i] == 0) break;
    }
    buf[1023] = 0;
    fprintf(stderr, "[OutputDebugString] %s\n", buf);
    // 🎯 The GSC pool-exhaustion errors arrive here as ONE multi-line string
    // ("server script runtime error\nexceeded maximum …\nTerminal script error").
    // This is the only point guaranteed to be on the failing path — the
    // allocator interpose demonstrably is not. Dump the pool while it is still
    // in the failed state.
    if (strstr(buf, "exceeded maximum number of child")) bo2_dump_child_pools(buf);
    win32_stdcall_ret(s, 1);
}

static void impl_MulDiv(State& s) {
    int32_t a = (int32_t)win32_arg(s, 0);
    int32_t b = (int32_t)win32_arg(s, 1);
    int32_t c = (int32_t)win32_arg(s, 2);
    if (c == 0) {
        s.gpr.rax.dword = 0xFFFFFFFF; // -1 on divide by zero
    } else {
        int64_t result = ((int64_t)a * b) / c;
        s.gpr.rax.dword = (uint32_t)result;
    }
    win32_stdcall_ret(s, 3);
}

static void impl_ExitProcess(State& s) {
    uint32_t code = win32_arg(s, 0);
    // __remill_function_call already popped the return addr (rsp += 4).
    // So [esp] now = the original [esp+4] = first arg (exit code).
    // The original return addr from the caller was the value we just popped.
    // arg0 is at win32_arg(s, 0) which reads esp+4 NOW = the value AFTER pop,
    // i.e. the next stack slot. Let me recompute properly.
    // Stack at __remill_function_call entry: [esp]=ret_addr, [esp+4]=arg0
    // We popped [esp] (the ret_addr). So esp+4 is now arg0, esp+8 is arg1.
    // win32_arg(0) reads esp+4 (the popped state) — that's the original ret+4.
    // Wait, win32_arg reads after pop. So win32_arg(0) = the right arg.
    fprintf(stderr, "[EXIT] ExitProcess(%u) esp=0x%08x\n", code, s.gpr.rsp.dword);
    // exit() leaves the caller frames on the stack, so scan upward for code-like
    // return addresses to reconstruct the WHO-called-exit path. EBP chain first,
    // then a raw scan (many CRT frames are frameless and the EBP walk skips them).
    fprintf(stderr, "[EXIT] EBP chain:\n");
    uint32_t bp = s.gpr.rbp.dword;
    for (int i = 0; i < 20 && bp >= 0x1000 && bp < 0xFFFFF000; i++) {
        uint32_t ret = win32_mem_read32(bp + 4), saved = win32_mem_read32(bp);
        fprintf(stderr, "    bp=0x%08x ret=0x%08x\n", bp, ret);
        if (saved <= bp) break;
        bp = saved;
    }
    fprintf(stderr, "[EXIT] stack scan (code-like return addrs, esp..+0x300):\n");
    for (int i = 0; i < 192; i++) {
        uint32_t v = win32_mem_read32(s.gpr.rsp.dword + i*4);
        if (v >= 0x401000u && v < 0xea0000u)
            fprintf(stderr, "    [esp+0x%03x] 0x%08x\n", i*4, v);
    }
    // 🎯 SORTIR SANS DEROULER L'HOTE. `exit()` fait tourner les destructeurs
    // statiques et les handlers atexit de l'HOTE pendant que les threads hotes
    // et les coroutines invitees sont encore vivants ; l'un d'eux leve, la levee
    // traverse une frontiere `noexcept`, et libc++ imprime
    //   « libc++abi: terminate_handler unexpectedly threw an exception »
    // suivi d'un signal 6. Une sortie PROPRE du moteur — MessageBoxA fatal puis
    // ExitProcess(0) — se lisait donc comme un plantage de NOTRE portage, et le
    // message du moteur (« LUI: Out of memory », « free does not match
    // allocation ») disparaissait sous le bruit du crash. Mesure 2026-09-17 :
    // quatre runs -O2 sur quatre, tous finis en signal 6 sur ce chemin.
    //
    // ExitProcess ne deroule rien non plus sur Windows : elle termine le
    // processus. On vide les tampons — c'est tout ce dont on a besoin, ne pas
    // perdre la fin du journal — puis on sort sans destructeurs.
    fflush(nullptr);
    std::_Exit((int)code);
}

static void impl_DebugBreak(State& s) {
    // DIAGNOSTIC: log + continue instead of SIGTRAP. The game calls this from
    // assertion macros (e.g. FUN_00927b00: `if (callback()==0) DebugBreak()`,
    // firing because callback 0x92b4c0 is unlifted -> shim returns 0). Continuing
    // reveals whether the assertion is recoverable or the value is truly needed.
    fprintf(stderr, "[DebugBreak] called (assertion) — continuing instead of SIGTRAP\n");
    win32_stdcall_ret(s, 0);
}

static void impl_RaiseException(State& s) {
    // RaiseException(dwExceptionCode, dwExceptionFlags, nNumberOfArguments, lpArguments)
    uint32_t code  = win32_arg(s, 0);
    uint32_t flags = win32_arg(s, 1);
    uint32_t nargs = win32_arg(s, 2);
    uint32_t argp  = win32_arg(s, 3);
    uint32_t ret   = win32_mem_read32(s.gpr.rsp.dword);
    fprintf(stderr, "[RaiseException] code=0x%08x flags=0x%x nargs=%u args@0x%08x ret=0x%08x",
            code, flags, nargs, argp, ret);
    for (uint32_t i = 0; i < nargs && i < 8; i++)
        fprintf(stderr, " a%u=0x%08x", i, argp ? win32_mem_read32(argp + i * 4) : 0);
    fprintf(stderr, "\n");
    if (code == 0x406D1388) {
        // MSVC thread-naming exception: raised inside
        // __try { RaiseException(...) } __except(EXCEPTION_EXECUTE_HANDLER) {}
        // purely to signal a debugger. Without a debugger the handler swallows
        // it and execution falls out of the __try — returning emulates that.
        fprintf(stderr, "[RaiseException] thread-name exception swallowed, continuing\n");
        win32_stdcall_ret(s, 4);
        return;
    }
    // --- fs-base sanity: lifted code reads FS base from State.addr.fs_base,
    // NOT from __remill_symbolic_FSBASE(). If the boot never set it, it is 0
    // and every fs:[disp] hits linear `disp`, not the TEB at 0x7FFD0000.
    uint32_t fsb = s.addr.fs_base.dword;
    fprintf(stderr, "[RaiseException] State.addr.fs_base=0x%08x  (symbolic_FSBASE=0x%08x)\n",
            fsb, __remill_symbolic_FSBASE());
    fprintf(stderr, "  TEB via fs_base: [+0]=0x%08x [+0x18]=0x%08x [+0x30]=0x%08x\n",
            win32_mem_read32(fsb + 0), win32_mem_read32(fsb + 0x18), win32_mem_read32(fsb + 0x30));
    fprintf(stderr, "  TEB via 0x7ffd0000: [+0]=0x%08x [+0x18]=0x%08x [+0x30]=0x%08x\n",
            win32_mem_read32(0x7ffd0000), win32_mem_read32(0x7ffd0018), win32_mem_read32(0x7ffd0030));

    // --- READ-ONLY forensic dump: answer (1) is there a catch on the fs:[0]
    // chain, (2) who called the thrower (EBP chain), (3) what C++ type is
    // thrown (ThrowInfo -> TypeDescriptor name). Nothing here mutates guest
    // state; we still abort at the end.
    auto code_like = [](uint32_t v){ return v >= 0x401000 && v < 0xEA0000; };

    // (1) SEH chain: fs:[0] -> {Next, Handler}. A C++ catch frame's handler is
    // __CxxFrameHandler (0x8ce410 in this binary). If one is on the chain, the
    // throw is EXPECTED to be caught -> normal EH. If the chain reaches the
    // -1 terminator with only a top-level/unhandled handler, it's unhandled.
    fprintf(stderr, "[RaiseException] fs:[0] SEH chain (fsbase=0x%08x):\n", fsb);
    uint32_t rec = win32_mem_read32(fsb + 0);
    for (int i = 0; i < 32 && rec != 0xFFFFFFFF && rec >= 0x1000; i++) {
        uint32_t next = win32_mem_read32(rec);
        uint32_t f4   = win32_mem_read32(rec + 4);
        uint32_t f8   = win32_mem_read32(rec + 8);
        fprintf(stderr, "  rec@0x%08x next=0x%08x +4=0x%08x%s +8=0x%08x%s\n",
                rec, next, f4, code_like(f4) ? "(code)" : "", f8, code_like(f8) ? "(code)" : "");
        rec = next;
    }
    fprintf(stderr, "  chain end: 0x%08x%s\n", rec, rec == 0xFFFFFFFF ? " (-1 terminator)" : "");

    // (2) EBP-chain call stack -> who called the thrower.
    fprintf(stderr, "[RaiseException] EBP call chain (ret addrs):\n");
    uint32_t bp = s.gpr.rbp.dword;
    for (int i = 0; i < 24 && bp >= 0x1000 && bp < 0xFFFFF000; i++) {
        uint32_t saved = win32_mem_read32(bp);
        uint32_t ret2  = win32_mem_read32(bp + 4);
        fprintf(stderr, "  bp=0x%08x ret=0x%08x%s\n", bp, ret2, code_like(ret2) ? "(code)" : "");
        if (saved <= bp) break;  // frames grow upward; stop on garbage
        bp = saved;
    }

    // (3) Thrown C++ type: pThrowInfo = 3rd EH arg. Layout (x86):
    // ThrowInfo{attrs, pmfnUnwind, pForwardCompat, pCatchableTypeArray};
    // CatchableTypeArray{nTypes, types[]}; CatchableType{props, pType, ...};
    // TypeDescriptor{pVFTable, spare, char name[]}  (name at +8).
    uint32_t pThrowInfo = (nargs >= 3 && argp) ? win32_mem_read32(argp + 8) : 0;
    fprintf(stderr, "[RaiseException] pExceptionObject=0x%08x pThrowInfo=0x%08x\n",
            (nargs >= 2 && argp) ? win32_mem_read32(argp + 4) : 0, pThrowInfo);
    if (pThrowInfo) {
        uint32_t pCTA = win32_mem_read32(pThrowInfo + 12);
        uint32_t nT   = pCTA ? win32_mem_read32(pCTA) : 0;
        fprintf(stderr, "  ThrowInfo{attrs=0x%08x cta=0x%08x nTypes=%u}\n",
                win32_mem_read32(pThrowInfo), pCTA, nT);
        for (uint32_t t = 0; t < nT && t < 6; t++) {
            uint32_t pCT   = win32_mem_read32(pCTA + 4 + t * 4);
            uint32_t pType = pCT ? win32_mem_read32(pCT + 4) : 0;
            char nm[128] = {0};
            if (pType) for (int k = 0; k < 127; k++) {
                char c = (char)win32_mem_read8(pType + 8 + k);
                nm[k] = c; if (!c) break;
            }
            fprintf(stderr, "  catchable[%u] type='%s'\n", t, nm);
        }
    }
    fprintf(stderr, "[RaiseException] unhandled exception\n");
    abort();
}

// --- STUBS (return 0, pop the right number of args) ---
#define STUB0(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 0); }
#define STUB1(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 1); }
#define STUB2(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 2); }
#define STUB3(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 3); }
#define STUB4(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 4); }
#define STUB5(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 5); }
#define STUB6(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 6); }
#define STUB7(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 7); }
#define STUB8(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 8); }
#define STUB9(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 9); }
#define STUB10(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 10); }
#define STUB_RET1_1(name) static void impl_##name(State& s) { s.gpr.rax.dword = 1; win32_stdcall_ret(s, 1); }
#define STUB_HANDLE(name, n) static void impl_##name(State& s) { s.gpr.rax.dword = ++t_fake_handle_counter; win32_stdcall_ret(s, n); }

static void impl_CreateMutexA(State& s) {                 // (lpAttr, bInitialOwner, lpName)
    if (!real_mutex_on()) { s.gpr.rax.dword = ++t_fake_handle_counter; win32_stdcall_ret(s, 3); return; }
    const uint32_t init = win32_arg(s, 1), pname = win32_arg(s, 2);
    std::string name;
    for (uint32_t i = 0; pname && i < 260; i++) { const uint8_t c = win32_mem_read8(pname + i); if (!c) break; name += (char)c; }
    std::lock_guard<std::mutex> lk(g_mutex_mx);                    // P661/C15
    if (!name.empty()) {
        auto it = g_mutex_names.find(name);
        if (it != g_mutex_names.end()) { t_last_error = 183; s.gpr.rax.dword = it->second; win32_stdcall_ret(s, 3); return; }  // ERROR_ALREADY_EXISTS
    }
    const uint32_t h = ++t_fake_handle_counter;
    GMutex& M = g_mutexes[h];
    if (init) { M.owner = g_cur; M.count = 1; }
    if (!name.empty()) g_mutex_names[name] = h;
    t_last_error = 0;
    fprintf(stderr, "[MUTEX] CreateMutexA h=0x%x initialOwner=%u name=\"%s\" tid=%d\n", h, init, name.c_str(), (int)g_cur);
    s.gpr.rax.dword = h;
    win32_stdcall_ret(s, 3);
}
STUB_HANDLE(CreateSemaphoreA, 4)
STUB_HANDLE(OpenProcess, 3)
STUB_HANDLE(LoadLibraryA, 1)
STUB_HANDLE(LoadLibraryW, 1)
static void impl_GetModuleHandleA_real(State& s) {
    fprintf(stderr, "[GMHA] arg=0x%08x -> 0x00400000\n", win32_arg(s, 0));
    s.gpr.rax.dword = 0x00400000;
    win32_stdcall_ret(s, 1);
}
static void impl_GetModuleHandleW_real(State& s) {
    fprintf(stderr, "[GMHW] arg=0x%08x -> 0x00400000\n", win32_arg(s, 0));
    s.gpr.rax.dword = 0x00400000;
    win32_stdcall_ret(s, 1);
}
static void impl_ReleaseMutex(State& s) {
    const uint32_t h = win32_arg(s, 0);
    std::lock_guard<std::mutex> lk(g_mutex_mx);                    // P661/C15
    auto it = real_mutex_on() ? g_mutexes.find(h) : g_mutexes.end();
    if (it == g_mutexes.end()) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 1); return; }     // the old stub
    GMutex& M = it->second;
    if (M.owner == g_cur && M.count) {
        if (--M.count == 0) M.owner = -1;
        s.gpr.rax.dword = 1;
    } else {
        t_last_error = 288;                                            // ERROR_NOT_OWNER
        static int shown = 0;
        if (shown++ < 6) fprintf(stderr, "[MUTEX] tid %d released h=0x%x it does not own (owner tid %d)\n", (int)g_cur, h, M.owner);
        s.gpr.rax.dword = 0;
    }
    win32_stdcall_ret(s, 1);
}
STUB3(ReleaseSemaphore)             // (hSemaphore, lReleaseCount, lpPreviousCount)
STUB1(ReleaseDLL)
STUB1(FreeLibrary)
STUB4(WaitForMultipleObjects)
// P803 : un talon qui rendait 0 = ECHEC ; Windows rend TRUE et garde la priorite. Le jeu la pose sur un fil cree
// suspendu, avant ResumeThread : Sound Mix TIME_CRITICAL (0x542ae4), Sound Decode et WebM ABOVE_NORMAL (0x4f4209,
// 0x4bb93d), Main TIME_CRITICAL le temps d'un etalonnage (0x83ea35/0x83eb4e). host_thread_entry la lit (BO2_PRIO_QOS).
static int prio_idx(uint32_t h) { return h == 0xFFFFFFFEu ? (int)g_cur : th_find(h); }
static void impl_SetThreadPriority(State& s) {
    const uint32_t h = win32_arg(s, 0);
    const int32_t p = (int32_t)win32_arg(s, 1);
    win32_stdcall_ret(s, 2);
    const int idx = prio_idx(h);
    if (idx >= 0 && idx < (int)sched_nthreads()) g_threads[idx]->prio = p;
    static int n = 0;
    if (n++ < 32) fprintf(stderr, "[SetThreadPriority] fil %d prio=%d (appel tid=%d)\n", idx, p, (int)g_cur);
    s.gpr.rax.dword = 1;
}
static void impl_GetThreadPriority(State& s) {
    const int idx = prio_idx(win32_arg(s, 0));
    win32_stdcall_ret(s, 1);
    s.gpr.rax.dword = idx >= 0 && idx < (int)sched_nthreads() ? (uint32_t)g_threads[idx]->prio : 0u;
}
STUB2(SetPriorityClass)
STUB2(SetProcessAffinityMask)
static void impl_GetProcessAffinityMask(State& s);   // C20 : defini avec GetSystemInfo
static void impl_GetVersionExA(State& s) {
    // GetVersionExA(LPOSVERSIONINFOEXA) — 1 stdcall arg. The STUB1 returned
    // eax=0 (FALSE) and left the struct UNINITIALIZED; the game checks `if
    // (!GetVersionExA(...)) Com_Error("Couldn't get OS info")` right before
    // its window setup, surfacing as the current Fatal Error. Fill plausible
    // Windows 7 SP1 values so the game's OS-version gate passes.
    // OSVERSIONINFOEXA layout (156 bytes): size + major + minor + build +
    // platformId(2=WIN32_NT) + szCSDVersion[128] + spMajor + spMinor +
    // suiteMask + productType(1=NT_WORKSTATION) + reserved.
    uint32_t p = win32_arg(s, 0);
    uint32_t sz = win32_mem_read32(p);
    win32_mem_write32(p + 4, 6);              // dwMajorVersion (Windows 7)
    win32_mem_write32(p + 8, 1);              // dwMinorVersion
    win32_mem_write32(p + 12, 7601);          // dwBuildNumber (Win7 SP1)
    win32_mem_write32(p + 16, 2);             // dwPlatformId = VER_PLATFORM_WIN32_NT
    // szCSDVersion[128] = "Service Pack 1\0..."
    const char* sp = "Service Pack 1";
    for (int i = 0; sp[i] && i < 127; i++) win32_mem_write8(p + 20 + i, (uint8_t)sp[i]);
    win32_mem_write8(p + 20 + 14, 0);
    if (sz >= 156) {  // OSVERSIONINFOEXA has the extra fields
        win32_mem_write32(p + 148, 1);        // wServicePackMajor (high word) — write32 packs both
        win32_mem_write32(p + 152, 0);        // wSuiteMask + wProductType + wReserved
        win32_mem_write8(p + 154, 1);         // wProductType = VER_NT_WORKSTATION
    }
    s.gpr.rax.dword = 1;                      // TRUE
    win32_stdcall_ret(s, 1);
}
STUB1(GlobalMemoryStatus)
STUB5(GetProcessTimes)              // (h, creation, exit, kernel, user)
STUB1(SetThreadExecutionState)      // (esFlags)
STUB1(SetErrorMode)
STUB1(SetUnhandledExceptionFilter)
STUB1(UnhandledExceptionFilter)
// AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler):
// stdcall, 2 args, returns a handle (PVOID). The old STUB1 was doubly wrong —
// it returned 0 (NULL) AND popped only 1 arg. The anti-tamper self-init at
// 0x8ce190 treats a NULL return as failure (`test eax,eax; je return-0`) and
// bails the whole startup, and the 1-vs-2 arg pop left the guest stack skewed.
// Return a non-NULL fake handle and clean 2 args.
// P925 : la campagne aussi (lecture du profil 4a5f70 par l'adaptateur x86) ; pas t6mp/t6zm en ligne (sans adaptateur).
extern "C" uint32_t bo2_add_veh(uint32_t,uint32_t);
extern "C" uint32_t bo2_remove_veh(uint32_t);
static void impl_AddVectoredExceptionHandler(State& s) {
    s.gpr.rax.dword = bo2_add_veh(win32_arg(s,0),win32_arg(s,1));
    win32_stdcall_ret(s, 2);
}
static void impl_RemoveVectoredExceptionHandler(State& s) {
    s.gpr.rax.dword = bo2_remove_veh(win32_arg(s,0));
    win32_stdcall_ret(s,1);
}
// GetProcAddress: return a function sentinel based on the resolved name.
// Each named function gets a unique sentinel (0xDEAD0001..0xDEAD0FFF) that
// is dispatched by __remill_function_call to a real impl.
static uint32_t g_gpa_counter = 0xDEAD0100;
// Real TLS/FLS impls (bitmap-backed per-thread slot arrays) are defined further
// below, after this dispatch table. FlsAlloc/FlsGetValue/FlsSetValue/FlsFree are
// reached through g_dyn_imports via the forward declarations just below.
static void impl_CorExitProcess(State& s) {
    // CorExitProcess(exitCode) — no-op (no .NET), just returns.
    // The CRT uses this for clean exit; if we don't call ExitProcess the
    // CRT proceeds to the user main.
    win32_stdcall_ret(s, 1);
}
static void impl_MessageBoxW(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 4); }
static void impl_GetActiveWindow_dyn(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 0); }
static void impl_GetLastActivePopup(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 1); }
static void impl_GetUserObjectInformationW_dyn(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 5); }
static void impl_SetProcessDPIAware(State& s) {
    // SetProcessDPIAware() — 0 args, mark DPI-aware, return TRUE. Real impl so
    // GetProcAddress stops handing back the 0xDEAD00FF poison the game then calls.
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 0);
}
static void impl_SystemFunction036(State& s) {
    // SystemFunction036 == RtlGenRandom(PVOID buf, ULONG len) — 2 stdcall args.
    // Fill buf with deterministic pseudo-random bytes (LCG, reproducible boot),
    // return TRUE, and clean BOTH args (the poison path only popped the retaddr
    // -> an 8-byte guest-stack leak).
    uint32_t buf = win32_arg(s, 0), len = win32_arg(s, 1);
    uint32_t x = 0x2545F491u ^ len;
    for (uint32_t i = 0; i < len; i++) {
        x = x * 1664525u + 1013904223u;
        win32_mem_write8(buf + i, (uint8_t)(x >> 24));
    }
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}
static void impl_GlobalMemoryStatusEx(State& s) {
    // GlobalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer) — 1 stdcall arg. The poison
    // path left the 64-byte struct UNINITIALIZED, so the game computed garbage
    // memory sizes -> garbage pointer calls -> crash. Fill plausible values.
    uint32_t p = win32_arg(s, 0);
    auto w64 = [&](uint32_t off, uint64_t v) {
        win32_mem_write32(p + off, (uint32_t)v);
        win32_mem_write32(p + off + 4, (uint32_t)(v >> 32));
    };
    win32_mem_write32(p + 4, 30);        // dwMemoryLoad = 30%  (dwLength at +0 left as caller set)
    w64(8,  0x200000000ull);             // ullTotalPhys      = 8 GB
    w64(16, 0x180000000ull);             // ullAvailPhys      = 6 GB
    w64(24, 0x400000000ull);             // ullTotalPageFile  = 16 GB
    w64(32, 0x300000000ull);             // ullAvailPageFile  = 12 GB
    w64(40, 0x80000000ull);              // ullTotalVirtual   = 2 GB (32-bit user space)
    w64(48, 0x60000000ull);              // ullAvailVirtual   = 1.5 GB
    w64(56, 0);                          // ullAvailExtendedVirtual
    s.gpr.rax.dword = 1;                 // TRUE
    win32_stdcall_ret(s, 1);
}
static void impl_DirectDrawCreateEx(State& s) {
    // DirectDrawCreateEx(GUID*, LPVOID*, REFIID, IUnknown*) — 4 stdcall args.
    // The poison path returned stale eax (could be 0 = DD_OK), so the game
    // believed DirectDraw was created, dereferenced the un-out-param, and
    // cascaded garbage indirect calls into Com_Error("x angle>"). We don't
    // emulate legacy DirectDraw (D3D11 path is the only renderer); return
    // DDERR_UNSUPPORTED (0x887601F7) and zero the out-param so the game's
    // failure check fires and it falls back to its no-DDraw path.
    uint32_t out_pp = win32_arg(s, 1);
    if (out_pp) win32_mem_write32(out_pp, 0);  // *lplpDD = NULL
    s.gpr.rax.dword = 0x887601F7;             // DDERR_UNSUPPORTED
    win32_stdcall_ret(s, 4);
}
static void impl_DirectDrawEnumerateExA(State& s) {
    // DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA, LPVOID, DWORD) — 3 stdcall
    // args. No DDraw drivers to enumerate; return DDERR_UNSUPPORTED so the
    // game treats "no DirectDraw" as a non-fatal condition and proceeds to
    // the D3D11 renderer. Importantly, do NOT call the callback (a guest
    // function pointer we'd have no safe way to invoke).
    s.gpr.rax.dword = 0x887601F7;             // DDERR_UNSUPPORTED
    win32_stdcall_ret(s, 3);
}

// Dispatch table for dynamically-resolved functions (sentinel → impl)
struct DynImport { const char* name; ImportImpl impl; };
// Forward decls (real impls defined below in the TLS/FLS section)
static void impl_FlsAlloc_real(State& s);
static void impl_FlsGetValue_real(State& s);
static void impl_FlsSetValue_real(State& s);
static void impl_FlsFree_real(State& s);
static void impl_GetSystemInfo(State& s);
static const DynImport g_dyn_imports[] = {
    {"FlsAlloc", impl_FlsAlloc_real},
    {"FlsGetValue", impl_FlsGetValue_real},
    {"FlsSetValue", impl_FlsSetValue_real},
    {"FlsFree", impl_FlsFree_real},
    {"CorExitProcess", impl_CorExitProcess},
    {"MessageBoxW", impl_MessageBoxW},
    {"GetActiveWindow", impl_GetActiveWindow_dyn},
    {"GetLastActivePopup", impl_GetLastActivePopup},
    {"GetUserObjectInformationW", impl_GetUserObjectInformationW_dyn},
    {"SetProcessDPIAware", impl_SetProcessDPIAware},
    {"SystemFunction036", impl_SystemFunction036},
    {"GlobalMemoryStatusEx", impl_GlobalMemoryStatusEx},
    {"DirectDrawCreateEx", impl_DirectDrawCreateEx},
    {"DirectDrawEnumerateExA", impl_DirectDrawEnumerateExA},
    // GetNativeSystemInfo has the SYSTEM_INFO signature and, on a non-WOW64
    // process, exactly GetSystemInfo's behaviour. libvpx's threading setup
    // (sub_8d89c0, reached from vp8dx_create_decompressor) resolves it by name
    // and calls it; before this entry existed the call fell through to the
    // generic stub and cost the VIDEO DECODER — see the note on GetProcAddress.
    {"GetNativeSystemInfo", impl_GetSystemInfo},
    {nullptr, nullptr}
};
// P661/C15 : plus de tables associatives. Le sentinel reste attribue dans l'ordre de
// resolution (0xDEAD0101 = le 1er resolu ; les journaux en dependent), mais la
// resolution, rare, prend un verrou, et la distribution — sur CHAQUE appel indirect
// hors IAT, depuis tous les fils — lit un tableau atomique sans verrou. L'unordered_map
// d'avant etait inseree paresseusement pendant que d'autres fils la lisaient (cf. C14).
static constexpr uint32_t kDynBase = 0xDEAD0101u;
static constexpr size_t kDynMax = 32;
static_assert(sizeof(g_dyn_imports) / sizeof(g_dyn_imports[0]) - 1 <= kDynMax, "kDynMax trop petit");
static std::atomic<uint32_t> g_dyn_sent[kDynMax];     // index dans g_dyn_imports -> sentinel (0 = pas encore)
static std::atomic<ImportImpl> g_dyn_impl[kDynMax];   // sentinel - kDynBase -> impl
static std::mutex g_dyn_mx;

static void impl_GetProcAddress(State& s) {
    uint32_t name_ptr = win32_arg(s, 1);
    char name_buf[128] = {0};
    if (name_ptr & 0x80000000) {
        snprintf(name_buf, sizeof(name_buf), "#%u", name_ptr & 0xFFFF);
    } else {
        for (int i = 0; i < 127; i++) {
            name_buf[i] = (char)win32_mem_read8(name_ptr + i);
            if (name_buf[i] == 0) break;
        }
    }
    fprintf(stderr, "[GPA] \"%s\"\n", name_buf);
    // Find by name
    for (int i = 0; g_dyn_imports[i].name; i++) {
        if (strcmp(name_buf, g_dyn_imports[i].name) == 0) {
            uint32_t sent = g_dyn_sent[i].load(std::memory_order_acquire);
            if (!sent) {
                std::lock_guard<std::mutex> lk(g_dyn_mx);
                sent = g_dyn_sent[i].load(std::memory_order_relaxed);
                if (!sent) {
                    sent = ++g_gpa_counter;
                    g_dyn_impl[sent - kDynBase].store(g_dyn_imports[i].impl, std::memory_order_release);
                    g_dyn_sent[i].store(sent, std::memory_order_release);
                }
            }
            s.gpr.rax.dword = sent;
            win32_stdcall_ret(s, 2);
            return;
        }
    }
    // ⚠️ UNKNOWN EXPORT: RETURN NULL, WHICH IS WHAT WINDOWS DOES.
    //
    // This used to hand back a non-null sentinel (0xDEAD00FF). That is worse
    // than useless in two compounding ways, and it cost the video decoder:
    //  * a caller that checks the result (the documented, common pattern) is
    //    told the function EXISTS and calls it, instead of taking its fallback;
    //  * the call then reaches __remill_function_call, matches nothing, prints
    //    ONE "unknown target 0xdead00ff" line and returns — WITHOUT POPPING THE
    //    STDCALL ARGUMENTS. The guest's esp stays low by 4 per argument for the
    //    rest of that frame, so the enclosing function's `pop esi/edi/ebx` read
    //    one slot off and it returns with SHIFTED CALLEE-SAVED REGISTERS.
    // That is what broke the WebM decoder: libvpx resolves GetNativeSystemInfo,
    // calls the sentinel, and vp8dx_create_decompressor comes back with
    // esi/edi/ebx shifted, so its caller stores the new decoder through a
    // corrupted `edi`, sees pbi == 0 and marks the video permanently dead. The
    // symptom was four levels away from the cause and nothing crashed.
    // A NULL is the honest answer and lets the guest's own fallback run (here:
    // GetSystemInfo instead of GetNativeSystemInfo).
    static uint32_t s_unknown = 0;
    if (s_unknown++ < 40)
        fprintf(stderr, "[GPA] \"%s\" NOT IMPLEMENTED -> returning NULL "
                        "(the guest's own fallback path runs)\n", name_buf);
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 2);
}

// Called from win32_runtime to dispatch dynamically-resolved calls
extern "C" bool win32_dispatch_dyn(uint32_t sentinel, State* state_ptr) {
    const uint32_t k = sentinel - kDynBase;
    if (k < kDynMax) {
        const ImportImpl f = g_dyn_impl[k].load(std::memory_order_acquire);
        if (f) { f(*state_ptr); return true; }
    }
    return false;
}
// Interlocked* — real read-modify-write on guest memory. The cooperative
// scheduler never preempts mid-shim, so a plain RMW is genuinely atomic here.
// A GUEST SPINLOCK MUST YIELD HERE, OR IT DEADLOCKS THE WHOLE PORT.
//
// MEASURED: `[RUNAWAY] IAT slot 0xa3b0c4 called 40000 times in a row` —
// InterlockedExchange, hammered by one thread with no other import in between.
// That is a spin-acquire loop. On Windows the holder is running on another core
// and releases within microseconds; here EVERY GUEST THREAD IS A COROUTINE ON
// ONE OS THREAD, so the spinner holds the only core and the holder can never
// run. The loop is then infinite by construction and the runaway guard kills the
// process.
// Giving the CPU back after a few failed attempts is the faithful translation,
// not a workaround: it reproduces what the hardware provides for free.
// `PAUSE`-then-yield is what a real spinlock does on a single core anyway.
extern "C" void win32_coop_yield();
extern "C" State* win32_current_guest_state();
static void spin_yield_check(uint32_t target) {
    // P642/B : PAR FIL HOTE. Partagee, la serie d'un fil heberge remettait a zero
    // celle de l'ordonnanceur a chaque appel entrelace (autre tid) : le spinner
    // du tourniquet n'atteignait plus 64 et ne cedait jamais.
    static thread_local uint32_t last_target = 0;
    static thread_local int      last_tid = -1, streak = 0;
    const int tid = win32_current_tid();
    if (target == last_target && tid == last_tid) {
        if (++streak >= 64 && (streak % 64) == 0) {
            // Yielding alone did NOT clear it — measured, the runaway guard still
            // fires at 40000 consecutive calls. So the holder is not merely
            // unscheduled, it is blocked or gone. Say what the spin is waiting
            // ON: the lock word, its current value, and the guest return address.
            // "It spins" is not a diagnosis; the address it spins on is.
            static uint32_t said = 0;
            if ((streak % 4096) == 0 && said++ < 12)
                fprintf(stderr, "[SPIN] tid=%d spinning on 0x%08x (value=0x%08x) "
                                "after %d attempts, ret=0x%08x\n",
                        tid, target, win32_mem_read32(target), streak,
                        win32_mem_read32(win32_current_guest_state()
                                         ? win32_current_guest_state()->gpr.rsp.dword : 0));
            win32_coop_yield();        // let whoever holds it make progress
        }
    } else {
        last_target = target; last_tid = tid; streak = 0;
    }
}
// 🔴 P642/A — INDIVISIBLES ENTRE FILS HOTES. « Le tourniquet ne preempte jamais au
// milieu d'un shim, donc lire-puis-ecrire est atomique » : vrai tant qu'un seul fil
// hote execute l'invite. Des qu'une coroutine est hebergee, deux coeurs font ces
// lectures-modifications-ecritures EN MEME TEMPS et en perdent — un compteur de
// references qui derive. Windows les fait `lock xadd`/`lock cmpxchg`. On les met
// sous LE verrou des instructions `lock` invitees (__remill_atomic_begin, mode 2,
// reentrant par fil hote) : exclusion avec les autres shims ET avec le code leve.
// Porte `g_any_hosted`, capturee a la construction : cout NUL en cooperatif.
// spin_yield_check reste HORS de la sequence : ceder en tenant le verrou
// bloquerait le fil heberge jusqu'au retour de la coroutine.
extern "C" void* __remill_atomic_begin(void* mem);
extern "C" void* __remill_atomic_end(void* mem);
#if defined(__ANDROID__)
// Android : la rayure de l'adresse cible, comme les sequences du code leve (win32_runtime.cpp, rayures).
extern "C" void bo2_atomic_lock_at(uint32_t a);
extern "C" void* bo2_atomic_end_at(void* mem);
struct IlkSeq {
    const bool on;
    explicit IlkSeq(uint32_t a) : on(__builtin_expect(g_any_hosted, 0)) { if (on) bo2_atomic_lock_at(a); }
    ~IlkSeq() { if (on) bo2_atomic_end_at(nullptr); }
};
#else
struct IlkSeq {
    const bool on;
    explicit IlkSeq(uint32_t) : on(__builtin_expect(g_any_hosted, 0)) { if (on) __remill_atomic_begin(nullptr); }
    ~IlkSeq() { if (on) __remill_atomic_end(nullptr); }
};
#endif
// 🎯 C24 (2026-09-26) — InterlockedExchange SUR UN LOCAL DE L'APPELANT N'EST QU'UNE BARRIERE.
// La file de taches sans verrou (0x8fc240, 0x8fc330, 0x8f9f20, 0x8fba37) appelle
// InterlockedExchange(&local, 0) a chaque tour : c'est MemoryBarrier() des vieux SDK.
// Sous Windows, un `xchg` sur une case de sa propre pile n'attend personne et ne cede
// jamais. Chez nous, chaque appel prenait LE verrou global des sequences `lock`
// (HW_ATOMICS=2, en concurrence avec 5 fils jq), puis passait par spin_yield_check.
// Comme la case locale a la meme adresse d'un appel a l'autre, spin_yield_check
// comptait une « serie » et faisait ceder Main tous les 64 appels.
// Une case de la pile du fil appelant, bornes lues dans SON TEB (fs:[8] limite, fs:[4]
// base, comme Windows les publie), n'est visible d'aucun autre fil. On garde la
// semantique de `xchg` : l'echange et une barriere complete. Sans le verrou, et sans
// cession, parce que ce n'est pas une attente.
static bool ilk_cible_sur_ma_pile(const State& s, uint32_t t) {
    const uint32_t teb = s.addr.fs_base.dword;
    if (!teb) return false;
    const uint32_t base = win32_mem_read32(teb + 0x04), limite = win32_mem_read32(teb + 0x08);
    return limite < base && t >= limite && t + 4u <= base;
}
// C33 : chemin materiel sous le drapeau du fil (voir win32_runtime.cpp, bo2_c33_ouvrir). Hors fenetre :
// ++g_guest_writes comme toute ecriture rapide (cas_spin_note s'en sert), puis spin_yield_check comme avant.
extern "C" uint32_t* bo2_c33_ouvrir(uint32_t a, uint32_t n);
extern "C" void bo2_c33_fermer(uint32_t* d);
extern "C" uint8_t* bo2_g_base;
extern "C" uint64_t g_guest_writes;
static inline uint32_t* c33_mot(uint32_t a) { return reinterpret_cast<uint32_t*>(bo2_g_base + a); }
static void impl_InterlockedExchange(State& s) {          // (Target, Value)
    uint32_t t = win32_arg(s, 0), v = win32_arg(s, 1);
    if (ilk_cible_sur_ma_pile(s, t)) {
        static uint32_t dit = 0;   // temoin unique ; lu et ecrit par plusieurs fils hotes
        if (!__atomic_exchange_n(&dit, 1u, __ATOMIC_RELAXED)) fprintf(stderr, "[C24] InterlockedExchange sur la pile de l'appelant (0x%08x, fs=0x%08x) : "
                                      "barriere sans verrou global ni cession\n", t, s.addr.fs_base.dword);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        s.gpr.rax.dword = win32_mem_read32(t);
        win32_mem_write32(t, v);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        win32_stdcall_ret(s, 2);
        return;
    }
    if (uint32_t* const d = bo2_c33_ouvrir(t, 4)) {
        s.gpr.rax.dword = __atomic_exchange_n(c33_mot(t), v, __ATOMIC_SEQ_CST);
        bo2_c33_fermer(d);
        ++g_guest_writes;
    } else { IlkSeq q(t);
      s.gpr.rax.dword = win32_mem_read32(t);
      win32_mem_write32(t, v); }
    spin_yield_check(t);
    win32_stdcall_ret(s, 2);
}
static void impl_InterlockedIncrement(State& s) {         // (Addend) -> new value
    uint32_t a = win32_arg(s, 0), v;
    if (uint32_t* const d = bo2_c33_ouvrir(a, 4)) {
        v = __atomic_add_fetch(c33_mot(a), 1u, __ATOMIC_SEQ_CST);
        bo2_c33_fermer(d);
        ++g_guest_writes;
    } else { IlkSeq q(a); v = win32_mem_read32(a) + 1; win32_mem_write32(a, v); }
    s.gpr.rax.dword = v;
    win32_stdcall_ret(s, 1);
}
static void impl_InterlockedDecrement(State& s) {         // (Addend) -> new value
    uint32_t a = win32_arg(s, 0), v;
    if (uint32_t* const d = bo2_c33_ouvrir(a, 4)) {
        v = __atomic_sub_fetch(c33_mot(a), 1u, __ATOMIC_SEQ_CST);
        bo2_c33_fermer(d);
        ++g_guest_writes;
    } else { IlkSeq q(a); v = win32_mem_read32(a) - 1; win32_mem_write32(a, v); }
    s.gpr.rax.dword = v;
    win32_stdcall_ret(s, 1);
}
static void impl_InterlockedCompareExchange(State& s) {   // (Destination, Exchange, Comperand)
    uint32_t d = win32_arg(s, 0), x = win32_arg(s, 1), c = win32_arg(s, 2);
    uint32_t old;
    if (uint32_t* const fl = bo2_c33_ouvrir(d, 4)) {
        old = c;
        const bool ok = __atomic_compare_exchange_n(c33_mot(d), &old, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        bo2_c33_fermer(fl);
        if (ok) ++g_guest_writes;
    } else { IlkSeq q(d); old = win32_mem_read32(d); if (old == c) win32_mem_write32(d, x); }
    s.gpr.rax.dword = old;
    if (old != c) spin_yield_check(d);   // failed CAS: this is a spin-acquire
    win32_stdcall_ret(s, 3);
}
static void impl_InterlockedExchangeAdd(State& s) {       // (Addend, Value) -> old value
    uint32_t a = win32_arg(s, 0), v = win32_arg(s, 1);
    uint32_t old;
    if (uint32_t* const d = bo2_c33_ouvrir(a, 4)) {
        old = __atomic_fetch_add(c33_mot(a), v, __ATOMIC_SEQ_CST);
        bo2_c33_fermer(d);
        ++g_guest_writes;
    } else { IlkSeq q(a); old = win32_mem_read32(a); win32_mem_write32(a, old + v); }
    s.gpr.rax.dword = old;
    win32_stdcall_ret(s, 2);
}       // NOTE: real arity is 2 — still mismatched, not in the CRT-path batch
// GetModuleHandleExA(dwFlags, lpModuleName, phModule). Un seul module ici : l'exe, a 0x400000 (comme
// GetModuleHandleA). Le STUB3 rendait FALSE ; zombies (0x4e8400, IPC Steam « DIPC ») le demande avec
// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS sur une adresse de son propre code, et abandonne au premier FALSE :
// WinMain sortait alors proprement (code 0). Windows rend le module qui contient l'adresse, sinon
// ERROR_MOD_NOT_FOUND. Bornes de l'image : SizeOfImage de l'en-tete PE, chargee a la base par boot.py.
static void impl_GetModuleHandleExA(State& s) {
    constexpr uint32_t kBase = 0x00400000u, kDepuisAdresse = 0x4u;
    const uint32_t drapeaux = win32_arg(s, 0), nom = win32_arg(s, 1), sortie = win32_arg(s, 2);
    uint32_t h = kBase;
    if (drapeaux & kDepuisAdresse) {
        const bool mz = (win32_mem_read32(kBase) & 0xFFFFu) == 0x5A4Du;
        const uint32_t taille = mz ? win32_mem_read32(kBase + win32_mem_read32(kBase + 0x3Cu) + 0x50u) : 0u;
        if (nom < kBase || nom - kBase >= taille) h = 0;
    }
    if (sortie) win32_mem_write32(sortie, h);
    if (!h) t_last_error = 126;                                       // ERROR_MOD_NOT_FOUND
    s.gpr.rax.dword = h ? 1u : 0u;
    win32_stdcall_ret(s, 3);
}
STUB7(FormatMessageA)               // (flags, src, msgId, langId, buf, size, args)
STUB1(GetDriveTypeA)                // (lpRootPathName)
STUB1(GetDriveTypeW)                // (lpRootPathName)
STUB2(GetWindowsDirectoryW)         // (lpBuffer, uSize) ; Zombies : vraie implementation
STUB2(GetSystemDirectoryW)          // (lpBuffer, uSize) ; Zombies : vraie implementation
STUB2(GetComputerNameA)             // (lpBuffer, nSize)
STUB0(GetConsoleWindow)             // (void)
STUB0(GetConsoleCP)                 // (void)
STUB2(GetConsoleMode)               // (hConsole, lpMode)
STUB2(SetConsoleMode)               // (hConsole, dwMode)
STUB1(FlushConsoleInputBuffer)
STUB4(ReadConsoleInputA)            // (h, lpBuffer, nLength, lpNumberRead)
STUB2(SetStdHandle)                 // (nStdHandle, hHandle)
STUB1(GetStdHandle)
// GetSystemInfo(lpSystemInfo) — was a no-op STUB, which left dwNumberOfProcessors
// at whatever happened to be in guest memory (zero). BO2 SIZES ITS WORKER POOL FROM
// IT: with no processors reported it creates none of Worker0..Worker7, and
// DB_DecompressIOStreamWorker is a JOB that runs on that pool — so image data read
// out of the ipaks was never decompressed and no streamed texture could ever be
// built. The thread table at 0xbe4130 shows exactly which threads came up
// (Backend, Server, Database, Stream, WebM Decode) and which never did (all eight
// Workers, Sound Mix, Sound Decode).
//
// SYSTEM_INFO (32-bit): wProcessorArchitecture +0, wReserved +2, dwPageSize +4,
// lpMinimumApplicationAddress +8, lpMaximumApplicationAddress +0xc,
// dwActiveProcessorMask +0x10, dwNumberOfProcessors +0x14, dwProcessorType +0x18,
// dwAllocationGranularity +0x1c, wProcessorLevel +0x20, wProcessorRevision +0x22.
// BO2_CPUS caps it: the guest scheduler is cooperative, so more workers is
// not automatically better, and being able to dial it is worth having.
// Un seul nombre de processeurs pour GetSystemInfo ET GetProcessAffinityMask : un
// Windows ne rend jamais deux valeurs differentes aux deux questions.
static uint32_t guest_ncpu() {
    static const uint32_t ncpu = [] {
        const char* e = bo2_getenv("BO2_CPUS");
        uint32_t v = e ? (uint32_t)strtoul(e, nullptr, 10) : 8u;
        return v ? (v > 32u ? 32u : v) : 1u;
    }();
    return ncpu;
}
static uint32_t guest_cpu_mask() {
    const uint32_t ncpu = guest_ncpu();
    return (ncpu >= 32) ? 0xffffffffu : ((1u << ncpu) - 1u);
}

// 🎯 C20 (2026-09-26) — GetProcessAffinityMask(hProcess, lpProcessMask, lpSystemMask)
// ETAIT UN STUB QUI N'ECRIVAIT RIEN, ET C'EST ELLE QUI DIMENSIONNE LA FILE DE TRAVAUX.
// 0x80dda0 : `push esp-slot ; push GetCurrentProcess() ; call [0xa3b1e4]`, puis
// `mov edx,[esp]` SANS tester le retour, masque brut range en [0x2925654] et son
// popcount (plafonne a 8) en [0x292557c]. 0x62b300 en tire le nombre de fils jq
// (c<=2 -> 1, c<=4 -> 2, c<=10 -> c-2, sinon 8), 0x5fb230 le masque d'ouvriers
// (1:0x4 2:0xc 3:0x1c 4:0x3c >=5:0x7c) qui ecrase [0xea7970] via 0x8f9cf0, et
// 0x8fbbb0/0x8fb960 creent un « JQ Worker %d » par bit (le bit 0 est Main).
// Avec le stub, le jeu comptait les bits d'une case de pile jamais ecrite : c=6
// dans nos runs, donc 4 ouvriers (tids 5-8, proc 0x8fb8e0), plus une lecture hors
// bornes a 0x5e2c10 que seul c=6 atteint. Un PC a 8 processeurs logiques rend
// 0xff : c=8, 5 ouvriers. Le dimensionnement par GetSystemInfo decrit plus bas ne
// concerne PAS ce pool (son OU a 0x8fa8a0 est ecrase par celui-ci).
static void impl_GetProcessAffinityMask(State& s) {
    const uint32_t mask = guest_cpu_mask();
    const uint32_t pp = win32_arg(s, 1), ps = win32_arg(s, 2);
    if (pp) win32_mem_write32(pp, mask);
    if (ps) win32_mem_write32(ps, mask);
    static int n = 0;
    if (++n <= 4)
        fprintf(stderr, "[AFFINITY] #%d GetProcessAffinityMask -> processus=0x%08x systeme=0x%08x "
                        "(%u processeurs) ret=0x%08x\n",
                n, mask, mask, guest_ncpu(), win32_mem_read32(s.gpr.rsp.dword));
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 3);
}

static void impl_GetSystemInfo(State& s) {
    uint32_t p = win32_arg(s, 0);
    if (p) {
        const uint32_t ncpu = guest_ncpu();
        win32_mem_write32(p + 0x00, 0);              // PROCESSOR_ARCHITECTURE_INTEL
        win32_mem_write32(p + 0x04, 4096);           // dwPageSize
        win32_mem_write32(p + 0x08, 0x00010000u);    // lpMinimumApplicationAddress
        // 0xFFFEFFFF, not 0x7FFEFFFF: t6sp.exe is IMAGE_FILE_LARGE_ADDRESS_AWARE
        // and a 32-bit large-address process under WOW64 gets the full 4 GiB user
        // range, which is what real Windows reports here. We were handing it the
        // 2 GiB value while the arena now extends to 0xF0000000, so any engine
        // check of an allocation against this limit would have rejected addresses
        // that are in fact perfectly valid.
        win32_mem_write32(p + 0x0c, 0xfffeffffu);    // lpMaximumApplicationAddress
        win32_mem_write32(p + 0x10, guest_cpu_mask());
        win32_mem_write32(p + 0x14, ncpu);           // dwNumberOfProcessors
        win32_mem_write32(p + 0x18, 586);            // dwProcessorType (PENTIUM)
        win32_mem_write32(p + 0x1c, 0x00010000u);    // dwAllocationGranularity
        win32_mem_write32(p + 0x20, 6);              // wProcessorLevel | revision
        static int said = 0;
        if (!said++ && bo2_getenv("BO2_STREAM_PROBE"))
            fprintf(stderr, "[GetSystemInfo] reporting %u processors\n", ncpu);
    }
    win32_stdcall_ret(s, 1);
}

// GetFileType(hFile) — the CRT's fopen() calls this right after CreateFileA and
// REJECTS the file (closes the handle, returns NULL) unless it reports a known
// type. Our handles are POSIX fds, so fstat() them: regular files -> DISK,
// char devices -> CHAR, pipes/sockets -> PIPE. A zero (UNKNOWN) return here is
// exactly what broke every stdio fopen — e.g. reading localization.txt, which
// starved the locale and left the localized zone name empty.
static void impl_GetFileType(State& s) {
    uint32_t h = win32_arg(s, 0);
    uint32_t type = 1;  // FILE_TYPE_DISK — safe default for handles we can't stat
    struct stat st;
    if ((int)h >= 0 && fstat((int)h, &st) == 0) {
        if (S_ISCHR(st.st_mode))                       type = 2;  // FILE_TYPE_CHAR
        else if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) type = 3;  // FILE_TYPE_PIPE
        else                                           type = 1;  // FILE_TYPE_DISK
    }
    s.gpr.rax.dword = type;
    win32_stdcall_ret(s, 1);
}
STUB6(PeekNamedPipe)                // (h, buf, size, read, avail, left)
STUB4(CreatePipe)                   // (readPipe, writePipe, sa, size)
STUB10(CreateProcessA)              // 10 args
// TerminateProcess(hProcess, uExitCode) — stdcall 2 args. Single-process, so
// any target means "exit". Must actually terminate: the CRT's unhandled-
// exception path calls this to die, and a returning stub recurses to a crash.
static void impl_TerminateProcess(State& s) {
    uint32_t code = win32_arg(s, 1);
    fprintf(stderr, "[EXIT] TerminateProcess(uExitCode=%u) esp=0x%08x\n", code, s.gpr.rsp.dword);
    bo2_guest_stack_of_runner("TerminateProcess");   // qui ferme : la pile invitee de chaque fil (Z-NATIF-13d)
    fprintf(stderr, "[EXIT] pile brute esp..+0x180 :");   // les mots tels quels : le balayage ci-dessus garde aussi des restes
    for (uint32_t i = 0; i < 96u; i++) fprintf(stderr, "%s%08x", i % 8u ? " " : "\n[EXIT]   ", win32_mem_read32(s.gpr.rsp.dword + i * 4u));
    fprintf(stderr, "\n");
    // 🎯 SORTIR SANS DEROULER L'HOTE. `exit()` fait tourner les destructeurs
    // statiques et les handlers atexit de l'HOTE pendant que les threads hotes
    // et les coroutines invitees sont encore vivants ; l'un d'eux leve, la levee
    // traverse une frontiere `noexcept`, et libc++ imprime
    //   « libc++abi: terminate_handler unexpectedly threw an exception »
    // suivi d'un signal 6. Une sortie PROPRE du moteur — MessageBoxA fatal puis
    // ExitProcess(0) — se lisait donc comme un plantage de NOTRE portage, et le
    // message du moteur (« LUI: Out of memory », « free does not match
    // allocation ») disparaissait sous le bruit du crash. Mesure 2026-09-17 :
    // quatre runs -O2 sur quatre, tous finis en signal 6 sur ce chemin.
    //
    // ExitProcess ne deroule rien non plus sur Windows : elle termine le
    // processus. On vide les tampons — c'est tout ce dont on a besoin, ne pas
    // perdre la fin du journal — puis on sort sans destructeurs.
    fflush(nullptr);
    std::_Exit((int)code);
}
STUB1(SuspendThread)
STUB2(GetExitCodeProcess)           // (hProcess, lpExitCode)
STUB7(DuplicateHandle)              // real arity 7 (was STUB1 -> leaked 24B/0x18 into WinMain's frame)
// 🎯 CreateDirectoryA WAS A STUB, AND THAT IS WHY ANGOLA NEVER TICKS.
//
// The chain, measured end to end 2026-08-29:
//   the game asks for <install>/players, this stub says "done" and creates
//   nothing -> CreateFileA("<install>/players/bindings_sp.bdg") FAILs ->
//   Com_Error("SaveDevice_PC : cannot open file for saving") -> 0x4446fe does
//   `mov dword[0x287bb30], 1` -> SV_Frame parks at 0x852b70 sleeping 50 ms for
//   as long as that word is non-zero and NEVER reaches its tick gate at
//   0x852bb9 -> every server counter reads +0, guestFrames stops, and the
//   render pump re-presents the last complete frame for ever.
//
// A stub that answers "success" is worse than one that answers "failure": the
// caller believes the directory exists and only finds out much later, somewhere
// unrelated. This creates it for real, exactly as the game asks and exactly
// where Windows would — no path is invented, win32_resolve_path does the
// mapping every other file call uses.
static void impl_CreateDirectoryA(State& s) {
    char path[1200];
    win32_resolve_path(win32_arg(s, 0), path, sizeof(path));
    win32_stdcall_ret(s, 2);
    const int rc = ::mkdir(path, 0755);
    const bool ok = (rc == 0) || (errno == EEXIST);
    static int n = 0;
    if (n++ < 12)
        fprintf(stderr, "[CreateDirectory] \"%s\" -> %s\n", path,
                rc == 0 ? "created" : (errno == EEXIST ? "already there" : strerror(errno)));
    s.gpr.rax.dword = ok ? 1u : 0u;          // BOOL
}
// 🎯 MoveFileA — THE LAST LINK. It was not implemented at all, and the runtime
// said so in a line nobody had read:
//     [unimplemented import called] MoveFileA (IAT slot 0x00a3b33c)
//
// The save writes players/temp.svg correctly (251,818 bytes, and the engine's own
// size check agrees), then sub_601130 promotes it: it builds both full paths and
// calls MoveFileA(temp, savegame) through 0x9658b8. With no implementation the
// call fails, savegame.svg never appears, sub_684c30 returns -1 for its size,
// 0x52a022 compares 251,818 against -1, and Com_Error(
// "GAME_SAVE_INSUFFICIENT_FREE_DISK") sets [0x287bb30]=1 — which parks SV_Frame
// at 0x852b70 for ever and is the whole of angola's advancing=NO.
//
// The error name is a red herring: nothing on that path measures disk space.
static void impl_MoveFileA(State& s) {
    char from[1200], to[1200];
    win32_resolve_path(win32_arg(s, 0), from, sizeof(from));
    win32_resolve_path(win32_arg(s, 1), to,   sizeof(to));
    win32_stdcall_ret(s, 2);
    int rc = ::rename(from, to);
    if (rc != 0 && errno == EXDEV) {           // across devices: copy then unlink
        FILE* a = ::fopen(from, "rb");
        FILE* b = a ? ::fopen(to, "wb") : nullptr;
        if (a && b) {
            char buf[65536]; size_t k;
            while ((k = ::fread(buf, 1, sizeof buf, a)) > 0) ::fwrite(buf, 1, k, b);
            rc = 0;
        }
        if (b) ::fclose(b);
        if (a) ::fclose(a);
        if (rc == 0) ::unlink(from);
    }
    static int n = 0;
    if (n++ < 12)
        fprintf(stderr, "[MoveFile] \"%s\" -> \"%s\" : %s\n", from, to,
                rc == 0 ? "moved" : strerror(errno));
    s.gpr.rax.dword = (rc == 0) ? 1u : 0u;     // BOOL
}
// 🎯 THIRD STUB THAT LIED, same family as CreateDirectoryA. The save writes
// temp.svg correctly (measured: 251,608 bytes, matching the expected size) and
// then has to be promoted to savegame.svg; the engine imports DeleteFileA and
// NOT MoveFile/CopyFile, so it does the promotion itself and deletes the old
// file first. A delete that answers "done" without deleting leaves the engine
// reading a stale or absent savegame.svg — measured: its size query returns -1,
// the sizes disagree at 0x52a022, and Com_Error("GAME_SAVE_INSUFFICIENT_FREE_DISK")
// parks SV_Frame exactly as the open failure used to.
static void impl_DeleteFileA(State& s) {
    char path[1200];
    win32_resolve_path(win32_arg(s, 0), path, sizeof(path));
    win32_stdcall_ret(s, 1);
    const int rc = ::unlink(path);
    static int n = 0;
    if (n++ < 12)
        fprintf(stderr, "[DeleteFile] \"%s\" -> %s\n", path,
                rc == 0 ? "deleted" : strerror(errno));
    s.gpr.rax.dword = (rc == 0) ? 1u : 0u;
}
STUB2(SetFileAttributesA)           // (lpFileName, dwFileAttributes)
STUB6(FindFirstFileExA)             // (name, infoLevel, findData, searchOp, filter, flags)
STUB2(FindNextFileA)                // (hFindFile, lpFindFileData)
STUB1(FindClose)
STUB4(GetFullPathNameW)             // (name, len, buf, filePart) ; Zombies : vraie implementation (P711)
STUB2(SetEnvironmentVariableA)      // (lpName, lpValue)
STUB2(SetEnvironmentVariableW)      // (lpName, lpValue)
STUB3(GetEnvironmentVariableA)      // (lpName, lpBuffer, nSize)
STUB3(GetEnvironmentVariableW)      // (lpName, lpBuffer, nSize)
STUB2(IsValidLocale)            // (Locale, dwFlags)
STUB2(EnumSystemLocalesA)       // (lpLocaleEnumProc, dwFlags)
STUB4(GetLocaleInfoA)           // (Locale, LCType, lpLCData, cchData)
STUB4(GetLocaleInfoW)
STUB0(GetUserDefaultLCID)       // (void)
STUB6(CompareStringW)           // (Locale, dwCmpFlags, s1, cch1, s2, cch2)
STUB6(LCMapStringW)             // (Locale, dwMapFlags, src, cchSrc, dest, cchDest)
STUB2(GetCPInfo)                // (CodePage, lpCPInfo)
STUB0(GetACP)                   // (void)
STUB0(GetOEMCP)                 // (void)
STUB1(IsValidCodePage)
STUB2(FileTimeToSystemTime)         // (lpFileTime, lpSystemTime)
STUB2(FileTimeToLocalFileTime)      // (lpFileTime, lpLocalFileTime)
STUB6(GetTimeFormatA)               // (locale, flags, time, fmt, buf, cch)
STUB6(GetDateFormatA)               // (locale, flags, date, fmt, buf, cch)
STUB1(GetTimeZoneInformation)
STUB3(OpenFileMappingA)             // (dwDesiredAccess, bInheritHandle, lpName)
STUB3(OpenEventA)                   // (dwDesiredAccess, bInheritHandle, lpName)
STUB1(UnmapViewOfFile)
// PulseEvent: real impl above (wake-token pulse delivery for the coop scheduler)
STUB4(HeapSetInformation)       // (HeapHandle, InfoClass, Info, InfoLength)
STUB5(HeapQueryInformation)         // (h, class, buf, len, retlen)
STUB1(GlobalLock)
STUB1(GlobalUnlock)

// --- allocator-backed buffer/string returns ------------------------------
// These used to stub to NULL/0, starving the CRT of argv[0] and the environ
// block. They now write into a guest buffer and return a guest address.

// Path reported for the main module (argv[0]); any non-empty path satisfies
// the CRT's _setargv. ASCII for the A variant.
#if defined(BO2_TARGET_MPS)
static const char kModulePath[] = "C:\\t6mp.exe";   // exes Steam : leur nom dans l'installation Steam
#elif defined(BO2_TARGET_ZMS)
static const char kModulePath[] = "C:\\t6zm.exe";
#else
static const char kModulePath[] = "C:\\t6sp.exe";
#endif

static void impl_GetModuleFileNameA(State& s) {
    // GetModuleFileNameA(hModule, lpFilename, nSize) — stdcall 3 args
    uint32_t buf  = win32_arg(s, 1);
    uint32_t size = win32_arg(s, 2);
    if (buf == 0 || size == 0) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 3); return; }
    uint32_t n = (uint32_t)strlen(kModulePath);
    uint32_t copy = n < size - 1 ? n : size - 1;
    for (uint32_t i = 0; i < copy; i++) win32_mem_write8(buf + i, (uint8_t)kModulePath[i]);
    win32_mem_write8(buf + copy, 0);
    // Win32: returns chars copied (excl. null); on truncation returns nSize.
    s.gpr.rax.dword = (copy < n) ? size : copy;
    win32_stdcall_ret(s, 3);
}

static void impl_GetModuleFileNameW(State& s) {
    // GetModuleFileNameW(hModule, lpFilename, nSize) — nSize counts WCHARs.
    uint32_t buf  = win32_arg(s, 1);
    uint32_t size = win32_arg(s, 2);
    if (buf == 0 || size == 0) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 3); return; }
    uint32_t n = (uint32_t)strlen(kModulePath);
    uint32_t copy = n < size - 1 ? n : size - 1;
    for (uint32_t i = 0; i < copy; i++) {
        win32_mem_write8(buf + i*2, (uint8_t)kModulePath[i]);
        win32_mem_write8(buf + i*2 + 1, 0);
    }
    win32_mem_write8(buf + copy*2, 0);
    win32_mem_write8(buf + copy*2 + 1, 0);
    s.gpr.rax.dword = (copy < n) ? size : copy;
    win32_stdcall_ret(s, 3);
}

// Current directory = the exe dir "C:\" (consistent with kModulePath). The engine
// derives fs_basepath / the fastfile search path from these; STUB1 returned 0 so
// the search path was empty and zone/*.ff were never opened. win32_resolve_path
// maps "C:\..." to the real game dir, so any path built from this resolves right.
static const char kCwd[] = "C:\\";
static void impl_GetCurrentDirectoryA(State& s) {
    uint32_t size = win32_arg(s, 0), buf = win32_arg(s, 1);
    uint32_t n = (uint32_t)strlen(kCwd);
    if (buf && size > n) { for (uint32_t i = 0; i <= n; i++) win32_mem_write8(buf + i, (uint8_t)kCwd[i]); s.gpr.rax.dword = n; }
    else s.gpr.rax.dword = n + 1;
    win32_stdcall_ret(s, 2);
}
static void impl_GetCurrentDirectoryW(State& s) {
    uint32_t size = win32_arg(s, 0), buf = win32_arg(s, 1);
    uint32_t n = (uint32_t)strlen(kCwd);
    if (buf && size > n) { for (uint32_t i = 0; i <= n; i++) { win32_mem_write8(buf + i*2, (uint8_t)kCwd[i]); win32_mem_write8(buf + i*2 + 1, 0); } s.gpr.rax.dword = n; }
    else s.gpr.rax.dword = n + 1;
    win32_stdcall_ret(s, 2);
}
// GetFullPathName(lpFileName, nBufferLength, lpBuffer, lpFilePart) — canonicalize:
// relative -> prepend CWD; copy to lpBuffer; set lpFilePart to the basename.
static void impl_GetFullPathNameA(State& s) {
    uint32_t in = win32_arg(s, 0), len = win32_arg(s, 1), buf = win32_arg(s, 2), part = win32_arg(s, 3);
    char raw[1024]; int i = 0; for (; i < 1023; i++) { char c = (char)win32_mem_read8(in + i); raw[i] = c; if (!c) break; } raw[1023] = 0;
    char full[1100];
    if (raw[0] && raw[1] == ':' && (raw[2] == 0 || (raw[2] == '.' && raw[3] == 0)))
        snprintf(full, sizeof(full), "%s", kCwd);                                // "X:" / "X:." -> CWD (C:\)
    else if (raw[0] && raw[1] == ':' && raw[2] == '\\')
        snprintf(full, sizeof(full), "%s", raw);                                 // absolute
    else if (raw[0] && raw[1] == ':')
        snprintf(full, sizeof(full), "%s%s", kCwd, raw + 2);                     // drive-relative -> CWD + rest
    else
        snprintf(full, sizeof(full), "%s%s", kCwd, raw);                         // relative -> CWD + path
    uint32_t n = (uint32_t)strlen(full);
    fprintf(stderr, "[GetFullPathNameA] \"%s\" -> \"%s\"\n", raw, full);
    if (buf && len > n) {
        for (uint32_t j = 0; j <= n; j++) win32_mem_write8(buf + j, (uint8_t)full[j]);
        if (part) { const char* sl = strrchr(full, '\\'); win32_mem_write32(part, sl ? buf + (uint32_t)(sl - full) + 1 : buf); }
        s.gpr.rax.dword = n;
    } else s.gpr.rax.dword = n + 1;
    win32_stdcall_ret(s, 4);
}

// A minimal but valid process environment. The CRT's _wenvinit scans a UTF-16
// block of "NAME=VALUE\0" entries terminated by an extra null WCHAR.
static const char* const kGuestEnvVars[] = {
    "ALLUSERSPROFILE=C:\\ProgramData",
    "SystemDrive=C:",
    "SystemRoot=C:\\Windows",
    "TEMP=C:\\Temp",
    "TMP=C:\\Temp",
    "USERPROFILE=C:\\Users\\Player",
    "windir=C:\\Windows",
};
// Write `str` as a UTF-16LE run incl. its null WCHAR at guest addr p; returns
// bytes written.
static uint32_t guest_write_wcstr(uint32_t p, const char* str) {
    uint32_t i = 0;
    for (; str[i]; i++) {
        win32_mem_write8(p + i*2, (uint8_t)str[i]);
        win32_mem_write8(p + i*2 + 1, 0);
    }
    win32_mem_write8(p + i*2, 0);
    win32_mem_write8(p + i*2 + 1, 0);
    return (i + 1) * 2;
}
// VPX_SIMD_CAPS — libvpx's OWN documented override, and the reason the WebM
// video can decode at all. `x86_simd_caps()` (sub_8d88d0) reads this variable
// with getenv BEFORE it runs cpuid, and returns its value verbatim; the run-time
// dispatch tables are then filled from it. remill cannot lift libvpx's
// hand-written SSE2 kernels — the loop filter at 0x8f6b24 (psubsb / paddsb /
// pcmpeqb on xmm) lands in __remill_error and kills the decode thread — so the
// plain-C variants, which lift fine, are the only ones that can run here.
// 0 = no SIMD at all. This is the library's supported switch, not a patch.
// BO2_VPX_SIMD=<value> overrides it (empty string disables the entry entirely,
// restoring cpuid-based detection).
static std::string guest_vpx_caps_entry() {
    const char* e = bo2_getenv("BO2_VPX_SIMD");
    if (e && !*e) return std::string();        // explicitly disabled
    return std::string("VPX_SIMD_CAPS=") + (e ? e : "0");
}
static void impl_GetEnvironmentStringsW(State& s) {
    // GetEnvironmentStringsW(void) — stdcall 0 args. Build once, cache.
    static uint32_t block = 0;
    if (!block) {
        const std::string vpx = guest_vpx_caps_entry();
        uint32_t total = 2;  // final block-terminator null WCHAR
        for (auto v : kGuestEnvVars) total += ((uint32_t)strlen(v) + 1) * 2;
        if (!vpx.empty()) total += ((uint32_t)vpx.size() + 1) * 2;
        block = guest_alloc(total, 4);
        if (block) {
            uint32_t p = block;
            for (auto v : kGuestEnvVars) p += guest_write_wcstr(p, v);
            if (!vpx.empty()) p += guest_write_wcstr(p, vpx.c_str());
            win32_mem_write8(p, 0); win32_mem_write8(p + 1, 0);  // double-null end
        }
    }
    s.gpr.rax.dword = block;
    win32_stdcall_ret(s, 0);
}
static void impl_FreeEnvironmentStringsW(State& s) {
    // FreeEnvironmentStringsW(lpszEnvironmentBlock) — arena never frees; TRUE.
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 1);
}

// --- TLS / FLS — real per-thread slot implementation ---
// TLS: 64 slots, bitmap allocator. Single-threaded is sufficient for CRT init.
// FLS: 128 slots (Vista+ Fiber Local Storage — same concept, more slots).
static constexpr int TLS_MAX_SLOTS = 64;
static constexpr int FLS_MAX_SLOTS = 128;
static uint32_t g_tls_bitmap = 0;  // bit i = slot i allocated
static uint32_t g_fls_bitmap = 0;
// Index 0 is the PE static-TLS index (loader-assigned; see pe_tls_new_block) —
// TlsAlloc must never hand it out or the CRT's ptd slot would overwrite the
// game's __declspec(thread) block pointer.
extern "C" uint32_t win32_tls_bitmap_reserve0() { g_tls_bitmap |= 1u; return 0; }

// TLS values live in each guest thread's GUEST-MEMORY slot array (tls_va, the
// block its TEB's fs:[0x2c] points at): the game bypasses the Tls* imports and
// reads fs:[0x2c] directly (0x80df30/0x502ff0 cached-tid checks), so API
// writes MUST land in the same array the direct reads hit. Host-side-only
// storage desynced them and every thread saw main's cached tid.
static uint32_t tls_slot_va(int tid, uint32_t idx) {
    return g_threads[tid]->tls_va + idx * 4;
}

// TlsAlloc (0 args): returns DWORD TLS index, or TLS_OUT_OF_INDEXES (0xFFFFFFFF)
static void impl_TlsAlloc_real(State& s) {
    sched_init();
    for (int i = 0; i < TLS_MAX_SLOTS; i++) {
        if (!(g_tls_bitmap & (1u << i))) {
            g_tls_bitmap |= (1u << i);
            for (size_t t = 0; t < sched_nthreads(); t++)     // Windows zeroes the
                win32_mem_write32(tls_slot_va((int)t, i), 0); // slot in all threads
            s.gpr.rax.dword = (uint32_t)i;
            win32_stdcall_ret(s, 0);
            return;
        }
    }
    s.gpr.rax.dword = 0xFFFFFFFF;
    win32_stdcall_ret(s, 0);
}
// TlsFree (1 arg): free a TLS index. Returns BOOL.
static void impl_TlsFree_real(State& s) {
    uint32_t idx = win32_arg(s, 0);
    sched_init();
    if (idx < TLS_MAX_SLOTS && (g_tls_bitmap & (1u << idx))) {
        g_tls_bitmap &= ~(1u << idx);
        for (size_t t = 0; t < sched_nthreads(); t++)
            win32_mem_write32(tls_slot_va((int)t, idx), 0);
        s.gpr.rax.dword = 1;
    } else {
        s.gpr.rax.dword = 0;
    }
    win32_stdcall_ret(s, 1);
}
// TlsGetValue (1 arg): returns LPVOID stored at slot — PER GUEST THREAD (the CRT
// keeps its per-thread data pointer here; a global slot corrupted it across
// coroutines and spun __getptd_noexit).
static void impl_TlsGetValue_real(State& s) {
    uint32_t idx = win32_arg(s, 0);
    sched_init();
    s.gpr.rax.dword = (idx < TLS_MAX_SLOTS) ? win32_mem_read32(tls_slot_va(g_cur, idx)) : 0;
    win32_stdcall_ret(s, 1);
}
// TlsSetValue (2 args): store LPVOID at slot — PER GUEST THREAD.
static void impl_TlsSetValue_real(State& s) {
    uint32_t idx = win32_arg(s, 0);
    uint32_t val = win32_arg(s, 1);
    sched_init();
    if (idx < TLS_MAX_SLOTS) {
        win32_mem_write32(tls_slot_va(g_cur, idx), val);
        s.gpr.rax.dword = 1;
    } else {
        s.gpr.rax.dword = 0;
    }
    win32_stdcall_ret(s, 2);
}
// FlsAlloc (1 arg: callback): returns FLS index, or FLS_OUT_OF_INDEXES
static void impl_FlsAlloc_real(State& s) {
    for (int i = 0; i < FLS_MAX_SLOTS; i++) {
        if (!(g_fls_bitmap & (1u << i))) {
            g_fls_bitmap |= (1u << i);
            s.gpr.rax.dword = (uint32_t)i;
            win32_stdcall_ret(s, 1);
            return;
        }
    }
    s.gpr.rax.dword = 0xFFFFFFFF;
    win32_stdcall_ret(s, 1);
}
// FlsFree (1 arg): free FLS index. Returns BOOL.
static void impl_FlsFree_real(State& s) {
    uint32_t idx = win32_arg(s, 0);
    if (idx < FLS_MAX_SLOTS && (g_fls_bitmap & (1u << idx))) {
        g_fls_bitmap &= ~(1u << idx);
        s.gpr.rax.dword = 1;
    } else {
        s.gpr.rax.dword = 0;
    }
    win32_stdcall_ret(s, 1);
}
// FlsGetValue (1 arg) — per guest thread (API-only access, no direct fs reads)
// 🎯 INSTRUMENTED. [RUNAWAY] named a million-iteration CRT loop:
//     999,839 GetLastError + 999,839 SetLastError, and between them
//     `call eax` with eax = 0xdead0102.
// Sentinels are handed out in resolution order from g_gpa_counter = 0xDEAD0100,
// and the log's [GPA] lines resolve FlsAlloc(0101), **FlsGetValue(0102)**,
// FlsSetValue(0103), FlsFree(0104). So 0xdead0102 IS FlsGetValue, reached from
// the CRT's __getptd at 0x96d5ca:
//     call [GetLastError] ; push [flsindex] ; call 0x96d488 -> DecodePointer
//     call eax            ; <- FlsGetValue(flsindex)
//     test eax,eax / jne  ; non-null -> done
//     push 0x214 ; call   ; NULL -> allocate a fresh per-thread block
// __getptd is called by nearly every CRT function, so FlsGetValue returning 0
// forever means a 0x214-byte allocation per CRT call — which is the million.
//
// The read is per-guest-thread and looks right, so the question is whether the
// matching FlsSetValue ever stores, on the same thread and index. Log both,
// with tid and index, and count them for [CADENCE]. Cheap, and it separates
// "the store never happens" from "the store happens somewhere else".
uint64_t g_fls_get = 0, g_fls_set = 0, g_fls_get_null = 0;
static void impl_FlsGetValue_real(State& s) {
    uint32_t idx = win32_arg(s, 0);
    sched_init();
    const uint32_t v = (idx < FLS_MAX_SLOTS) ? g_threads[g_cur]->fls[idx] : 0;
    ++g_fls_get; if (!v) ++g_fls_get_null;
    static uint32_t n = 0;
    if (n++ < 8)
        fprintf(stderr, "[FLS] get idx=%u tid=%d -> 0x%08x%s\n", idx, (int)g_cur, v,
                v ? "" : "   <- NULL: the CRT will allocate another 0x214 block");
    s.gpr.rax.dword = v;
    win32_stdcall_ret(s, 1);
}
// FlsSetValue (2 args) — per guest thread
static void impl_FlsSetValue_real(State& s) {
    uint32_t idx = win32_arg(s, 0);
    uint32_t val = win32_arg(s, 1);
    sched_init();
    ++g_fls_set;
    { static uint32_t n = 0;
      if (n++ < 8) fprintf(stderr, "[FLS] set idx=%u tid=%d = 0x%08x\n", idx, (int)g_cur, val); }
    if (idx < FLS_MAX_SLOTS) {
        g_threads[g_cur]->fls[idx] = val;
        s.gpr.rax.dword = 1;
    } else {
        s.gpr.rax.dword = 0;
    }
    win32_stdcall_ret(s, 2);
}
// EncodePointer (1 arg) — identity (return the pointer unchanged)
static void impl_EncodePointer_real(State& s) {
    s.gpr.rax.dword = win32_arg(s, 0);
    win32_stdcall_ret(s, 1);
}
// DecodePointer (1 arg) — identity
static void impl_DecodePointer_real(State& s) {
    s.gpr.rax.dword = win32_arg(s, 0);
    win32_stdcall_ret(s, 1);
}
STUB1(GlobalSize)
STUB2(lstrcmpiW)                    // (lpString1, lpString2)
STUB4(RtlUnwind)                // (TargetFrame, TargetIp, ExceptionRecord, ReturnValue)
// IsProcessorFeaturePresent (1 arg). Le jeu ne demande que PF_XMMI64_INSTRUCTIONS_AVAILABLE (10), dans le CRT :
// 0x96c974 -> __sse2_available [0x4183ba8], 0x96d370 -> __use_sse2_mathfcns [0x4183ba0]. Windows repond 1 sur tout
// x86 SSE2. Le stub repondait 0 : ceil prenait le chemin x87 (FLDCW arrondi vers +inf, puis frndint), et notre FLDCW
// ignore l'arrondi -> ceil(0.3) = 0 -> sub_78d1c0 lit l'echantillon [-1] -> leurres de haiti a 1e31 (P756, P757).
static void impl_IsProcessorFeaturePresent(State& s) {
    s.gpr.rax.dword = win32_arg(s, 0) == 10u;
    win32_stdcall_ret(s, 1);
}
STUB0(GetVersion)               // (void)
STUB3(FindResourceW)                // (hModule, lpName, lpType)
STUB2(LoadResource)                 // (hModule, hResInfo)
STUB1(LockResource)
STUB2(SizeofResource)               // (hModule, hResInfo)
STUB2(ReportFault)                  // (pep, dwOpt)
// Proper-arity stubs replacing wrong-arity aliases (were pointed at
// impl_lstrlenA / impl_GetVersionExA in the table, which pop 1):
// Real narrow<->wide conversion for single-byte codepages (CP_ACP/CP_OEMCP/
// CP_UTF8 treated as Latin-1: each WCHAR <-> its low byte). Enough for the CRT's
// environment/locale conversion (__crtGetEnvironmentStringsA) and ASCII paths.
// Honors the -1 (NUL-terminated) length and the 0-size-query conventions.
static void impl_WideCharToMultiByte(State& s) {
    // (CodePage, dwFlags, lpWide, cchWide, lpMB, cbMB, lpDefaultChar, lpUsedDefault)
    uint32_t wide = win32_arg(s, 2);
    int32_t  cch  = (int32_t)win32_arg(s, 3);
    uint32_t mb   = win32_arg(s, 4);
    int32_t  cb   = (int32_t)win32_arg(s, 5);
    uint32_t count;
    if (cch < 0) {  // NUL-terminated: count includes the terminating NUL
        count = 0;
        for (;;) {
            uint32_t w = win32_mem_read8(wide + count*2) | (win32_mem_read8(wide + count*2 + 1) << 8);
            count++;
            if (w == 0) break;
        }
    } else {
        count = (uint32_t)cch;
    }
    if (cb == 0) {  // size query: 1 byte per WCHAR
        s.gpr.rax.dword = count;
        win32_stdcall_ret(s, 8);
        return;
    }
    uint32_t n = count < (uint32_t)cb ? count : (uint32_t)cb;
    for (uint32_t i = 0; i < n; i++)
        win32_mem_write8(mb + i, win32_mem_read8(wide + i*2));  // low byte
    s.gpr.rax.dword = n;
    win32_stdcall_ret(s, 8);
}
static void impl_MultiByteToWideChar(State& s) {
    // (CodePage, dwFlags, lpMB, cbMB, lpWide, cchWide)
    uint32_t mb   = win32_arg(s, 2);
    int32_t  cb   = (int32_t)win32_arg(s, 3);
    uint32_t wide = win32_arg(s, 4);
    int32_t  cch  = (int32_t)win32_arg(s, 5);
    uint32_t count;
    if (cb < 0) {  // NUL-terminated: count includes the terminating NUL
        count = 0;
        for (;;) { uint8_t c = win32_mem_read8(mb + count); count++; if (c == 0) break; }
    } else {
        count = (uint32_t)cb;
    }
    if (cch == 0) {  // size query: 1 WCHAR per byte
        s.gpr.rax.dword = count;
        win32_stdcall_ret(s, 6);
        return;
    }
    uint32_t n = count < (uint32_t)cch ? count : (uint32_t)cch;
    for (uint32_t i = 0; i < n; i++) {
        win32_mem_write8(wide + i*2, win32_mem_read8(mb + i));
        win32_mem_write8(wide + i*2 + 1, 0);
    }
    s.gpr.rax.dword = n;
    win32_stdcall_ret(s, 6);
}
STUB4(GetStringTypeW)           // (dwInfoType, lpSrcStr, cchSrc, lpCharType)
STUB1(SetHandleCount)           // (uNumber) — was unregistered (default popped 0)

// --- PSAPI (not KERNEL32 but dispatched by name) ---
// GetProcessMemoryInfo(handle, @counters, cb) — stdcall 3 args
// Uses a global for test-injected working set size. In production this
// would call the real proc_pidinfo on macOS.
static uint32_t g_test_ws = 0;
static uint32_t g_test_mem_ok = 1;
extern "C" void set_test_mem_params(uint32_t ok, uint32_t ws) { g_test_mem_ok = ok; g_test_ws = ws; }

static void impl_GetProcessMemoryInfo(State& s) {
    uint32_t counters_ptr = win32_arg(s, 1);
    if (g_test_mem_ok && counters_ptr) {
        // PROCESS_MEMORY_COUNTERS: WorkingSetSize is at offset 8
        win32_mem_write32(counters_ptr + 8, g_test_ws);
        s.gpr.rax.dword = 1; // BOOL success
    } else {
        s.gpr.rax.dword = 0;
    }
    win32_stdcall_ret(s, 3);
}

// --- REGISTRATION ---
// Map import name → implementation (ImportEntry is defined in win32_shim.h)

static const ImportEntry kernel32_impls[] = {
    {"GetProcessHeap", impl_GetProcessHeap},
    {"HeapAlloc", impl_HeapAlloc},
    {"HeapFree", impl_HeapFree},
    {"HeapCreate", impl_HeapCreate},
    {"HeapDestroy", impl_HeapDestroy},
    {"HeapSize", impl_HeapSize},
    {"GetCurrentThreadId", impl_GetCurrentThreadId},
    {"GetCurrentProcessId", impl_GetCurrentProcessId},
    {"GetCurrentThread", impl_GetCurrentThread},
    {"GetCurrentProcess", impl_GetCurrentProcess},
    {"Sleep", impl_Sleep},
    {"ExitThread", impl_ExitThread},
    {"GetTickCount", impl_GetTickCount},
    {"QueryPerformanceCounter", impl_QueryPerformanceCounter},
    {"QueryPerformanceFrequency", impl_QueryPerformanceFrequency},
    {"GetSystemTimeAsFileTime", impl_GetSystemTimeAsFileTime},
    {"GetSystemTime", impl_GetSystemTime},
    {"VirtualAlloc", impl_VirtualAlloc},
    {"VirtualFree", impl_VirtualFree},
    {"CreateFileA", impl_CreateFileA},
    {"ReadFile", impl_ReadFile},
    {"WriteFile", impl_WriteFile},
    {"CloseHandle", impl_CloseHandle},
    {"GetFileSize", impl_GetFileSize},
    {"GetFileTime", impl_GetFileTime},
    {"SetFilePointer", impl_SetFilePointer},
    {"InitializeCriticalSection", impl_InitializeCriticalSection},
    {"InitializeCriticalSectionAndSpinCount", impl_InitializeCriticalSectionAndSpinCount},
    {"DeleteCriticalSection", impl_DeleteCriticalSection},
    {"EnterCriticalSection", impl_EnterCriticalSection},
    {"LeaveCriticalSection", impl_LeaveCriticalSection},
    {"TryEnterCriticalSection", impl_TryEnterCriticalSection},
    {"GetLastError", impl_GetLastError},
    {"SetLastError", impl_SetLastError},
    {"lstrlenA", impl_lstrlenA},
    {"lstrlenW", impl_lstrlenW},
    {"IsDebuggerPresent", impl_IsDebuggerPresent},
    {"GetCommandLineA", impl_GetCommandLineA},
    {"OutputDebugStringA", impl_OutputDebugStringA},
    {"MulDiv", impl_MulDiv},
    {"ExitProcess", impl_ExitProcess},
    {"DebugBreak", impl_DebugBreak},
    {"RaiseException", impl_RaiseException},
    // stubs
    {"CreateEventA", impl_CreateEventA},
    {"CreateMutexA", impl_CreateMutexA},
    {"CreateSemaphoreA", impl_CreateSemaphoreA},
    {"CreateFileMappingA", impl_CreateFileMappingA},
    {"CreateThread", impl_CreateThread},
    {"OpenProcess", impl_OpenProcess},
    {"MapViewOfFile", impl_MapViewOfFile},
    {"LoadLibraryA", impl_LoadLibraryA},
    {"LoadLibraryW", impl_LoadLibraryW},
    {"GetModuleHandleA", impl_GetModuleHandleA_real},
    {"GetModuleHandleW", impl_GetModuleHandleW_real},
    {"SetEvent", impl_SetEvent},
    {"ResetEvent", impl_ResetEvent},
    {"ReleaseMutex", impl_ReleaseMutex},
    {"ReleaseSemaphore", impl_ReleaseSemaphore},
    {"FreeLibrary", impl_FreeLibrary},
    {"WaitForSingleObject", impl_WaitForSingleObject},
    {"WaitForMultipleObjects", impl_WaitForMultipleObjects},
    {"SetThreadPriority", impl_SetThreadPriority},
    {"GetThreadPriority", impl_GetThreadPriority},
    {"SetPriorityClass", impl_SetPriorityClass},
    {"SetProcessAffinityMask", impl_SetProcessAffinityMask},
    {"GetProcessAffinityMask", impl_GetProcessAffinityMask},
    {"GetVersionExA", impl_GetVersionExA},
    {"GlobalMemoryStatus", impl_GlobalMemoryStatus},
    {"GetProcessTimes", impl_GetProcessTimes},
    {"SetThreadExecutionState", impl_SetThreadExecutionState},
    {"SetErrorMode", impl_SetErrorMode},
    {"SetUnhandledExceptionFilter", impl_SetUnhandledExceptionFilter},
    {"UnhandledExceptionFilter", impl_UnhandledExceptionFilter},
    {"AddVectoredExceptionHandler", impl_AddVectoredExceptionHandler},
    {"RemoveVectoredExceptionHandler", impl_RemoveVectoredExceptionHandler},
    {"EncodePointer", impl_EncodePointer_real},
    {"DecodePointer", impl_DecodePointer_real},
    {"GetProcAddress", impl_GetProcAddress},
    {"InterlockedExchange", impl_InterlockedExchange},
    {"InterlockedIncrement", impl_InterlockedIncrement},
    {"InterlockedDecrement", impl_InterlockedDecrement},
    {"InterlockedCompareExchange", impl_InterlockedCompareExchange},
    {"InterlockedExchangeAdd", impl_InterlockedExchangeAdd},
    {"GetModuleFileNameA", impl_GetModuleFileNameA},
    {"GetModuleFileNameW", impl_GetModuleFileNameW},
    {"GetModuleHandleExA", impl_GetModuleHandleExA},
    {"FormatMessageA", impl_FormatMessageA},
    {"GetEnvironmentStringsW", impl_GetEnvironmentStringsW},
    {"FreeEnvironmentStringsW", impl_FreeEnvironmentStringsW},
    {"GetSystemInfo", impl_GetSystemInfo},
    {"GetDriveTypeA", impl_GetDriveTypeA},
    {"GetDriveTypeW", impl_GetDriveTypeW},
    {"GetWindowsDirectoryW", impl_GetWindowsDirectoryW},
    {"GetSystemDirectoryW", impl_GetSystemDirectoryW},
    {"GetComputerNameA", impl_GetComputerNameA},
    {"GetConsoleWindow", impl_GetConsoleWindow},
    {"GetConsoleCP", impl_GetConsoleCP},
    {"GetConsoleMode", impl_GetConsoleMode},
    {"SetConsoleMode", impl_SetConsoleMode},
    {"FlushConsoleInputBuffer", impl_FlushConsoleInputBuffer},
    {"ReadConsoleInputA", impl_ReadConsoleInputA},
    {"SetStdHandle", impl_SetStdHandle},
    {"GetStdHandle", impl_GetStdHandle},
    {"GetFileType", impl_GetFileType},
    {"PeekNamedPipe", impl_PeekNamedPipe},
    {"CreatePipe", impl_CreatePipe},
    {"CreateProcessA", impl_CreateProcessA},
    {"TerminateProcess", impl_TerminateProcess},
    {"SuspendThread", impl_SuspendThread},
    {"ResumeThread", impl_ResumeThread},
    {"GetExitCodeProcess", impl_GetExitCodeProcess},
    {"DuplicateHandle", impl_DuplicateHandle},
    {"CreateDirectoryA", impl_CreateDirectoryA},
    {"MoveFileA", impl_MoveFileA},
    {"DeleteFileA", impl_DeleteFileA},
    {"SetFileAttributesA", impl_SetFileAttributesA},
    {"GetFileAttributesA", impl_GetFileAttributesA},
    {"FindFirstFileExA", impl_FindFirstFileExA},
    {"FindNextFileA", impl_FindNextFileA},
    {"FindClose", impl_FindClose},
    {"GetFullPathNameA", impl_GetFullPathNameA},
    {"GetFullPathNameW", impl_GetFullPathNameW},
    {"GetCurrentDirectoryA", impl_GetCurrentDirectoryA},
    {"GetCurrentDirectoryW", impl_GetCurrentDirectoryW},
    {"SetEnvironmentVariableA", impl_SetEnvironmentVariableA},
    {"SetEnvironmentVariableW", impl_SetEnvironmentVariableW},
    {"GetEnvironmentVariableA", impl_GetEnvironmentVariableA},
    {"GetEnvironmentVariableW", impl_GetEnvironmentVariableW},
    {"IsValidLocale", impl_IsValidLocale},
    {"EnumSystemLocalesA", impl_EnumSystemLocalesA},
    {"GetLocaleInfoA", impl_GetLocaleInfoA},
    {"GetLocaleInfoW", impl_GetLocaleInfoW},
    {"GetUserDefaultLCID", impl_GetUserDefaultLCID},
    {"CompareStringW", impl_CompareStringW},
    {"LCMapStringW", impl_LCMapStringW},
    {"GetCPInfo", impl_GetCPInfo},
    {"GetACP", impl_GetACP},
    {"GetOEMCP", impl_GetOEMCP},
    {"IsValidCodePage", impl_IsValidCodePage},
    {"FileTimeToSystemTime", impl_FileTimeToSystemTime},
    {"FileTimeToLocalFileTime", impl_FileTimeToLocalFileTime},
    {"GetTimeFormatA", impl_GetTimeFormatA},
    {"GetDateFormatA", impl_GetDateFormatA},
    {"GetTimeZoneInformation", impl_GetTimeZoneInformation},
    {"OpenFileMappingA", impl_OpenFileMappingA},
    {"OpenEventA", impl_OpenEventA},
    {"UnmapViewOfFile", impl_UnmapViewOfFile},
    {"PulseEvent", impl_PulseEvent},
    {"HeapReAlloc", impl_HeapReAlloc},
    {"HeapSetInformation", impl_HeapSetInformation},
    {"HeapQueryInformation", impl_HeapQueryInformation},
    {"TlsAlloc", impl_TlsAlloc_real},
    {"TlsGetValue", impl_TlsGetValue_real},
    {"TlsSetValue", impl_TlsSetValue_real},
    {"TlsFree", impl_TlsFree_real},
    {"GlobalLock", impl_GlobalLock},
    {"GlobalUnlock", impl_GlobalUnlock},
    {"GlobalSize", impl_GlobalSize},
    {"lstrcmpiW", impl_lstrcmpiW},
    {"RtlUnwind", impl_RtlUnwind},
    {"IsProcessorFeaturePresent", impl_IsProcessorFeaturePresent},
    {"GetVersion", impl_GetVersion},
    {"FindResourceW", impl_FindResourceW},
    {"LoadResource", impl_LoadResource},
    {"LockResource", impl_LockResource},
    {"SizeofResource", impl_SizeofResource},
    {"ReportFault", impl_ReportFault},
    {"CreateSemaphoreW", impl_CreateSemaphoreA},
    {"CreateFileW", impl_CreateFileA},
    {"SetConsoleCtrlHandler", impl_SetErrorMode},
    {"GetStartupInfoW", impl_GetVersionExA},   // both arity 1 — alias is fine
    {"GetStringTypeW", impl_GetStringTypeW},
    {"WideCharToMultiByte", impl_WideCharToMultiByte},
    {"MultiByteToWideChar", impl_MultiByteToWideChar},
    {"SetHandleCount", impl_SetHandleCount},
    {"WriteConsoleW", impl_WriteFile},
    {"VirtualQuery", impl_VirtualQuery},
    {"GetProcessMemoryInfo", impl_GetProcessMemoryInfo},
    {nullptr, nullptr},
};

// public access for the dispatch init
static void bo2_start_stack_watch(void);
extern "C" const ImportEntry* get_kernel32_impls() {
    // Arm the host-clocked stack watch here: this runs once at startup, long
    // before any freeze, and does not depend on the guest pumping frames.
    bo2_start_stack_watch();
    return kernel32_impls;
}

// ── [STACK-CENSUS] — POLL EVERY GUEST STACK, BECAUSE THE HOOKS ARE NOT ON THE
//    PATH ────────────────────────────────────────────────────────────────────
//
// The 2026-08-25 [WATCHDOG crash] happens with `esp=0x4bfb0d60` inside
// sub_7fa710, which allocates a 14 KB local buffer:
//
//     0x7fa77a: mov  eax, 0x3880          ; 14,464 bytes
//     0x7fa77f: call 0x964030             ; chkstk — probe/commit that much
//     0x7fa784: lea  esi, [esp + 0x80]    ; the buffer  ← the crash crumb
//     0x7fa796: call 0x95eaa0             ; memset(esi, 0, 0x3800)
//
// [STACK-ESCAPE] and [STACK-DEEP] both stayed silent, and that is NOT evidence
// of a healthy stack: both fire from the IMPORT DISPATCHER, so a thread that
// runs deep inside lifted code without calling an import is invisible to them.
// This project has already paid for that mistake once — "a poll beats a hook
// that might not be on the path".
//
// So poll instead. Walking the thread table costs nothing on a periodic clock
// and reports every stack whether or not its owner ever calls an import.
// ⚠️ THE CENSUS WAS CADENCED BY THE RENDER LOOP, SO IT DIED WITH THE THING IT
// EXISTS TO MEASURE. Its only caller was a periodic block inside the frame pump
// (metal_backend.mm, gated on BO2_MT_AUDIT_EVERY). angola froze at 3,977 frames
// with the flag armed and the census printed NOTHING — because a frozen game
// stops pumping frames, so the poll stopped exactly when the stack was finally
// worth looking at. Two runs were spent before I noticed the clock was the bug.
//
// So drive it from a HOST thread that nothing in the guest can stall. This is
// the same lesson as "a poll beats a hook that might not be on the path", one
// level up: a poll on the WRONG CLOCK is just a hook with extra steps.
//
// BO2_STACK_WATCH=<seconds> (default 20; 0 disables).
extern "C" void win32_stack_census(const char* why);
extern "C" void win32_teardown_repair(void);
extern "C" void win32_selflink_scan(void);
extern "C" void win32_table_watch(void);
extern "C" void win32_slchain_scan(void);
extern "C" void win32_sl_devine_report(void);  // [SL-DEVINE], voir win32_runtime.cpp
extern "C" void win32_varchain_scan(void);   // [VARCHAIN], voir win32_runtime.cpp
extern "C" void bo2_internwho_report_changed(const char* why);
extern "C" void bo2_slfree_inflight_report(void);
extern "C" void bo2_varpool_trend(const char* why);
extern "C" void win32_slfree_who_report(void);

// Per-thread "last wait entered", written on every WaitForSingleObject and read
// by the host watch. Deliberately a plain fixed array with relaxed atomics: it
// has to stay readable when every guest thread is blocked and the scheduler is
// making no progress at all.
struct Bo2Wait { std::atomic<uint32_t> handle{0}, ret{0}, timeout{0}, seq{0}; };
static Bo2Wait g_bo2_wait[64];
static std::atomic<uint32_t> g_bo2_wait_seq{0};
void bo2_note_wait(int tid, uint32_t handle, uint32_t ret, uint32_t timeout) {
    if (tid < 0 || tid >= 64) return;
    g_bo2_wait[tid].handle.store(handle, std::memory_order_relaxed);
    g_bo2_wait[tid].ret.store(ret, std::memory_order_relaxed);
    g_bo2_wait[tid].timeout.store(timeout, std::memory_order_relaxed);
    g_bo2_wait[tid].seq.store(g_bo2_wait_seq.fetch_add(1, std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
}
extern "C" void bo2_gstack_current(const char* why);   // defini plus bas, ligne ~5784
static void bo2_report_waits(void) {
    static uint32_t last_seq[64] = {0};
    static int stuck_rounds = 0;
    char line[2048]; size_t n = 0; int moving = 0;
    for (int i = 0; i < 64; i++) {
        const uint32_t sq = g_bo2_wait[i].seq.load(std::memory_order_relaxed);
        if (!sq) continue;
        if (sq != last_seq[i]) { moving++; last_seq[i] = sq; }
        if (n < sizeof line - 96)
            n += snprintf(line + n, sizeof line - n, " tid%d:h=0x%x@0x%08x%s", i,
                          g_bo2_wait[i].handle.load(std::memory_order_relaxed),
                          g_bo2_wait[i].ret.load(std::memory_order_relaxed),
                          sq == last_seq[i] ? "" : "*");
    }
    if (!n) return;
    stuck_rounds = moving ? 0 : stuck_rounds + 1;
    fprintf(stderr, "[WAIT-MAP] %d thread(s) entered a new wait since last sample%s —%s\n",
            moving, stuck_rounds >= 2 ? " ⚠️ NONE FOR SEVERAL SAMPLES: DEADLOCK" : "", line);
    // 🎯 AND THE SCHEDULER'S OWN VIEW OF EACH THREAD. A handle alone cannot say
    // whether the thread that should SIGNAL it still exists. angola's main thread
    // waits on 0x1003c, which only tid 12 ever sets (11 times against main's 2) —
    // and tid 12 is absent from the wait map. Absent means either "running" or
    // "gone", and with every [CADENCE] counter at +0 nothing is running. Print
    // the status so the difference is never guessed again.
    if (stuck_rounds >= 2) {
        char st[1024]; size_t m = 0;
        for (size_t i = 0; i < sched_nthreads() && i < 64; i++) {
            const GThread* t = g_threads[i];
            if (!t) continue;
            const char* w = t->status == T_RUN       ? "RUN"
                          : t->status == T_BLOCKED   ? "BLOCKED"
                          : t->status == T_DONE      ? "DONE"
                                                     : "SUSPENDED";
            if (m < sizeof st - 48)
                m += snprintf(st + m, sizeof st - m, " tid%zu:%s(h=0x%x)", i, w, t->wait_h);
        }
        // 🎯 AND WHICH ONE HOLDS THE CPU. This is the whole answer when most
        // threads read RUN and nothing advances: the scheduler is cooperative, so
        // a single coroutine spinning inside lifted code — no import, no indirect
        // branch, no yield point — freezes every other thread and looks exactly
        // like a deadlock from outside. Name it, and its last known wait site.
        fprintf(stderr, "[THREAD-STATE] current=tid%d%s\n", (int)g_cur, st);
        fprintf(stderr, "[THREAD-STATE] ⚠️ most threads read RUN while every "
                        "[CADENCE] counter is +0 — that is NOT a deadlock, it is "
                        "tid%d spinning in lifted code and never yielding\n", (int)g_cur);
        // 🎯 ET MAINTENANT, OU. Le numero du thread ne nomme pas la fonction
        // invitee ; il a fallu un plantage et la lecture des miettes pour
        // arriver a une liste de cinq sites, dont [VARCHAIN] a depuis montre
        // qu'ils sont innocents (marche de seau saine, appelant borne a 16
        // tours). La pile INVITEE de la coroutine coincee est le seul temoin
        // qui reste, et ce fil-ci est hote : il parle quand l'invite ne
        // repond plus. Gratuit hors gel — on n'arrive ici qu'apres 40 s sans
        // qu'un seul thread entre dans une nouvelle attente.
        bo2_gstack_current("tid fige dans du code leve, vu par le fil de veille hote");
    }
}
static void bo2_start_stack_watch(void) {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    const char* e = bo2_getenv("BO2_STACK_WATCH");
    const int secs = e ? atoi(e) : 20;
    if (secs <= 0) return;
    std::thread([secs] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(secs));
            win32_stack_census("host-watch");
            win32_teardown_repair();
            win32_selflink_scan();
            win32_table_watch();
            win32_slchain_scan();
            // Le BILAN de [SL-DEVINE], et il doit etre appele meme a zero :
            // « 0 sur 22260 » est une reponse, le silence n'en est pas une.
            // C'est la faute que je viens de commettre en soudant la sonde
            // [TRUELEN-QUI] a une reparation qui n'a jamais tire.
            win32_sl_devine_report();
            // La chaine de seau de sub_479b90 n'a que deux sorties (cle
            // trouvee / suivant nul) et aucun import : un seau cyclique est
            // une boucle infinie sans point de cede. Ce fil-ci survit a
            // l'invite fige, donc il est le SEUL a pouvoir le nommer.
            win32_varchain_scan();
            // [IWHO] / [SANS-NUL] n'etaient imprimes qu'a la SORTIE du processus,
            // donc jamais dans un run qui gele — c'est-a-dire jamais dans le seul
            // cas qui nous interesse. Ce fil-ci survit a un invite bloque.
            bo2_internwho_report_changed("host-watch");
            // Le temoin d'appel EN VOL de SL_FreeString. Elle ne revient jamais
            // quand la cle n'est pas dans son seau, donc seule une sonde lue de
            // l'HOTE peut nommer son appelant. Porte son temoin negatif.
            bo2_slfree_inflight_report();
            // [SLFREE-WHO] etait DEFINI et JAMAIS APPELE : du code mort qui se
            // lisait comme une sonde silencieuse. Troisieme instrument de cette
            // session incapable d'atteindre son propre evenement.
            win32_slfree_who_report();
            // La reserve de variables GSC EN TENDANCE. Au gel il n'y a jamais
            // d'erreur d'epuisement — le thread est coince, pas en faute — donc
            // `bo2_varpool_report` n'est JAMAIS appele dans le seul cas qui
            // compte. Une ligne, seulement quand ca change, temoin negatif
            // compris. (Pas `bo2_varpool_report` : son propre commentaire dit
            // qu'un instantane hors de l'erreur n'est pas un diagnostic.)
            bo2_varpool_trend("host-watch");
            bo2_report_waits();
        }
    }).detach();
    fprintf(stderr, "[STACK-WATCH] host thread armed, every %ds "
                    "(BO2_STACK_WATCH=0 disables) — survives a frozen guest\n", secs);
}

// ── 🎯 BO2_TEXT_DUMP=<path> — DUMP THE CODE THAT IS ACTUALLY RUNNING ─────────
//
// ❌ RETRACTE LE 2026-09-18. Ce commentaire disait : « ce binaire est CEG (Steam
// Custom Executable Generation), le .text du disque n'est PAS le .text qui
// s'execute, ce qui explique que les xrefs de chaines renvoient zero reference ».
// LES DEUX MOITIES SONT FAUSSES, et elles ont coute du temps a qui les a crues :
//
//   * le .text du disque est octet pour octet IDENTIQUE a l'image vivante —
//     0 ecart sur 6 489 264 (AGENTS.md §4, mesure du 2026-08-29). Toute
//     l'analyse statique de ce projet en depend et elle marche.
//   * les xrefs vides ne viennent pas de CEG mais d'une LIMITE D'INSTRUMENT :
//     `tools/disasm.py --xref` ne trouve pas les appels RELATIFS. Mesure du
//     2026-09-18 : 0 reference pour sub_87c8e0, alors qu'on tient l'encodage
//     `e8 <rel32>` qui l'appelle depuis 0x5cacf3. Balayer les octets soi-meme.
//
// Le vidage ci-dessous reste utile pour confirmer qu'une page invitee porte bien
// les octets attendus ; il n'est simplement plus la seule source de verite.
//
// Ghidra and IDA both analyse the FILE, so neither can help with that — a better
// decompiler would decompile the wrong bytes. What DOES help is giving them the
// real thing: dump the mapped image once, from a HOST thread (nothing in the
// guest can stall it), and load the result at base 0x400000. Then the
// decompiler, the xrefs and the call graph all describe the code that runs.
//
// Written once per run, announced, and off by default.
extern "C" void win32_text_dump_once(void) {
    static const char* path = bo2_getenv("BO2_TEXT_DUMP");
    if (!path || !*path) return;
    static bool done = false;
    if (done) return;
    done = true;
    const uint32_t lo = 0x400000u, hi = 0xa40000u;      // the image as mapped
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[TEXT-DUMP] cannot open %s\n", path); return; }
    static uint8_t buf[0x10000];
    uint64_t written = 0;
    for (uint32_t a = lo; a < hi; a += sizeof buf) {
        const uint32_t n = (hi - a < sizeof buf) ? (hi - a) : (uint32_t)sizeof buf;
        for (uint32_t k = 0; k < n; k++) buf[k] = win32_mem_read8(a + k);
        written += fwrite(buf, 1, n, f);
    }
    fclose(f);
    fprintf(stderr, "\n🎯 [TEXT-DUMP] wrote %llu bytes of LIVE guest image 0x%08x..0x%08x to %s "
                    "— load it in Ghidra/IDA as raw x86-32 at base 0x%08x and the disassembly, "
                    "the xrefs and the decompiler finally describe the code that RUNS, not the "
                    "CEG-rewritten file on disk.\n\n",
            (unsigned long long)written, lo, hi, path, lo);
}
extern "C" void win32_stack_census(const char* why) {
    win32_text_dump_once();
    // ⚠️ THE BLIND SPOT THIS PROBE SHIPPED WITH. The first version did
    //     if (!t->guest_stack || !t->guest_stack_size) continue;
    // and said nothing about the threads it skipped — so a thread whose stack
    // was never recorded looked exactly like a thread with a healthy stack. The
    // 2026-08-25 [WATCHDOG crash] is on tid 10, inside _chkstk, and "every guest
    // stack is under 60%" was read as clearing tid 10 when tid 10 may never have
    // been looked at. Count them and name them.
    char line[2048]; size_t n = 0; int deep = 0;
    char unk[512]; size_t un = 0; int unknown = 0;
    for (size_t i = 0; i < sched_nthreads() && i < 64; i++) {
        GThread* t = g_threads[i];
        if (!t) continue;
        if (!t->guest_stack || !t->guest_stack_size) {
            ++unknown;
            if (un < sizeof(unk) - 16)
                un += snprintf(unk + un, sizeof(unk) - un, " tid%zu", i);
            continue;
        }
        State* st = t->cur_state;
        if (!st) continue;
        const uint32_t sp = st->gpr.rsp.dword;
        const uint32_t base = t->guest_stack, top = base + t->guest_stack_size;
        // ⚠️ A PARKED OR NEVER-STARTED COROUTINE HAS A MEANINGLESS ESP, AND IT
        // MUST NOT BE REPORTED AS AN ESCAPE. The first version printed
        //     tid10:OUTSIDE(esp=0xfffffff8 …, -1259521720 from base)
        // which reads as "tid 10 ran off its stack" and is nothing of the kind:
        // 0xfffffff8 is 0-8, a zeroed state with two pushes on it. Meanwhile the
        // crash dump's own esp=0x4bfb0d60 sits comfortably INSIDE tid 10's stack.
        // Reporting a stale saved state as a live escape is how this project has
        // twice built a session on a register that was never valid.
        if (sp < 0x10000u || sp >= 0xf0000000u) {
            ++unknown;
            if (un < sizeof(unk) - 40)
                un += snprintf(unk + un, sizeof(unk) - un, " tid%zu(esp=0x%08x:parked)", i, sp);
            continue;
        }
        if (sp < base || sp >= top) {
            // 🎯 PRINT THE NUMBERS. "OUTSIDE" alone says a thread escaped but not
            // by how much or in which direction, and those are the whole
            // diagnosis: a little below `base` is an overflow, far away is a
            // stack pointer that was never this thread's at all.
            n += snprintf(line + n, sizeof(line) - n,
                          " tid%zu:OUTSIDE(esp=0x%08x stack=0x%08x..0x%08x, %+d from base)",
                          i, sp, base, top, (int)(sp - base));
            ++deep; continue;
        }
        const uint32_t used = top - sp;
        const unsigned pct = (unsigned)((uint64_t)used * 100u / t->guest_stack_size);
        if (pct >= 60) {
            ++deep;
            n += snprintf(line + n, sizeof(line) - n, " tid%zu:%u%%(%uK/%uK)",
                          i, pct, used >> 10, t->guest_stack_size >> 10);
        }
        if (n > sizeof(line) - 64) break;
    }
    // ⚠️ Report the quiet case too. "No line" and "every stack is fine" must not
    // look the same, or the next session reads silence as a healthy answer —
    // which is exactly how [STACK-DEEP] misled this one.
    if (deep)
        fprintf(stderr, "[STACK-CENSUS] (%s) 🎯 %d stack(s) at/over 60%%:%s\n", why, deep, line);
    else
        fprintf(stderr, "[STACK-CENSUS] (%s) every guest stack it can see is under 60%% used\n", why);
    // Never let "not measured" hide inside "measured and fine".
    if (unknown)
        fprintf(stderr, "[STACK-CENSUS] (%s) ⚠️ %d thread(s) have NO recorded stack and were NOT "
                        "measured (no recorded stack, or a parked/never-started state whose "
                        "esp is meaningless):%s — any 'stacks are fine' verdict does not cover "
                        "them\n",
                why, unknown, unk);
}

// The State the running coroutine executes on — for the HWATCH fault handler
// (win32_runtime.cpp), which must read the guest register file without a lock.
extern "C" State* bo2_current_state() {
    return (g_cur >= 0 && (size_t)g_cur < sched_nthreads() && g_threads[g_cur]) ? g_threads[g_cur]->cur_state : nullptr;
}

// For [PHEAD] (win32_runtime.cpp): the running coroutine's guest call chain.
extern "C" void bo2_gstack_current(const char* why) { bo2_guest_stack_of(why, g_cur); }

// Scheduler switch count (every coop_yield that hands the thread to another coroutine).
extern "C" uint64_t win32_sched_switches() { uint64_t t = 0; for (int i = 0; i < 32; i++) t += g_pick_count[i]; return t; }

// For [RUN-THREADS]: how many guest longjmps each thread has thrown (the ring's counter).
extern "C" uint32_t win32_longjmp_count(int tid) { return (tid >= 0 && tid < 64) ? g_lj_n[tid] : 0u; }
