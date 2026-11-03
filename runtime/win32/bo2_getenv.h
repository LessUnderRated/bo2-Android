// bo2_getenv.h — MEMOISED getenv, because the environment cannot change.
//
// 🎯 WHY THIS EXISTS. L61 measured host libc's `__findenv_locked` at 23.16 ms
// PER TICK late in an angola run (×16.5 over early) — a linear scan over the
// ~40 BO2_* variables this project passes, taken under a lock, on a hot path.
// The traced terminal stall is a GSC string operator run ~53,147 times
// (sub_6138a0 → sub_571b20 → SL_GetString → sub_64d340 → the MT allocator), and
// `bo2_intern` called getenv() on EVERY intern — 106,295 scans in one frozen
// window. That is entirely our own tax: the guest never asked for it.
//
// ⚠️ THE PRECONDITION, CHECKED RATHER THAN ASSUMED: nothing under runtime/ calls
// setenv, putenv, unsetenv, or assigns to `environ` (grepped, zero hits). The
// answer to a lookup is therefore fixed for the life of the process, and the
// pointer getenv returns stays valid because nothing rewrites the block.
//
// ⚠️ BO2_GETENV_NOCACHE=1 RESTORES THE UNCACHED PATH IN THE SAME BINARY. An A/B
// on two builds cannot be shown afterwards to have compared two different
// binaries, and this repo has already retracted a bisect for exactly that. One
// binary, one flag.
//
// Keys must be stable string literals (all runtime callers were checked).
// Published slots are immutable. A colliding key falls back to libc rather than
// replacing a slot another native thread may be reading. Publishing only the key
// with release/acquire does NOT protect a subsequently overwritten value.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// 🎯 P653/C8 — LE CACHE RATAIT UN TIERS DES CLES. 512 cases adressees par
// `(pointeur >> 3) & 511`, SANS sondage, pour 368 cles et 532 sites : une cle
// dont la case etait prise retombait sur libc a CHAQUE appel, et lesquelles
// dependait de la disposition du binaire et de l'ordre du premier appel. Les
// profils P651b y mettent 9,6-17,5 % du fil ordonnanceur (libc getenv prend un
// verrou global). 4096 cases, hachage du pointeur entier, sondage lineaire : une
// cle trouve sa case ou la premiere case vide de sa suite. Une case n'est jamais
// liberee, donc une cle n'occupe qu'une case et une recherche s'arrete au vide.
namespace bo2_env_detail {
struct Slot {
    std::atomic<const char*> key{nullptr};
    const char* val = nullptr;
    std::atomic_flag claimed = ATOMIC_FLAG_INIT;
};
constexpr uint32_t kSlots = 4096;
inline Slot g_slots[kSlots];
inline std::atomic<uint64_t> g_libc{0};   // retombees sur libc, temoin [GETENV-LIBC]
inline const bool g_nocache = (::getenv("BO2_GETENV_NOCACHE") != nullptr);
// Compte, imprime aux puissances de deux, appelle libc. Hors du chemin chaud.
__attribute__((noinline, cold)) inline const char* libc_fallback(const char* name) {
    const uint64_t n = g_libc.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0) fprintf(stderr, "[GETENV-LIBC #%llu] %s\n", (unsigned long long)n, name);
    return ::getenv(name);
}
}  // namespace bo2_env_detail

inline const char* bo2_getenv(const char* name) {
    using namespace bo2_env_detail;
    if (__builtin_expect(g_nocache, 0)) return ::getenv(name);
    const uint32_t h = (uint32_t)(((uint64_t)(uintptr_t)name * 0x9E3779B97F4A7C15ull) >> 52);
    for (uint32_t i = 0; i < kSlots; i++) {
        Slot& s = g_slots[(h + i) & (kSlots - 1)];
        const char* key = s.key.load(std::memory_order_acquire);
        if (__builtin_expect(key == name, 1)) return s.val;
        if (key) continue;
        // Case vide : un seul ecrivain la possede. Perdre la prise ne bloque
        // jamais l'appelant (ni une recherche reentrante) et ne lit jamais un val
        // non publie : on repasse par libc, la prochaine recherche la trouvera.
        if (s.claimed.test_and_set(std::memory_order_relaxed)) return libc_fallback(name);
        const char* v = ::getenv(name);
        s.val = v;
        s.key.store(name, std::memory_order_release);
        return v;
    }
    return libc_fallback(name);
}

// 🎯 A FLAG THAT "=0" CANNOT TURN OFF IS NOT A FLAG.
//
// The idiom `bo2_getenv("X") ? 1 : 0` tests whether the variable EXISTS, so
// `X=0` reads as ON. That is harmless for a probe nobody disables, and a real
// defect for anything a run script pins: scripts/angola_run.sh sets BO2_WINDOW,
// BO2_XAUDIO, BO2_SND_UNMUTE, BO2_VPX_NATIVE, BO2_VPX_REPLACE, BO2_KB_PHYSICAL
// and BO2_ERR_CATALOGUE unconditionally, so passing `X=0` on the command line
// changed NOTHING and the A/B that appeared to have been run had not been.
// BO2_XAUDIO was fixed on its own once; this is the same bug in six more places.
//
// Unset keeps the caller's default, so no existing behaviour moves: the only
// inputs whose meaning changes are the ones that were silently ignored.
inline bool bo2_env_on(const char* name, bool dflt = false) {
    const char* v = bo2_getenv(name);
    if (!v || !*v) return dflt;
    return strcmp(v, "0") != 0 && strcmp(v, "off") != 0 &&
           strcmp(v, "false") != 0 && strcmp(v, "no") != 0;
}
