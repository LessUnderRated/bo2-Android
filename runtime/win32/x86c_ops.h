// Opérations du code produit par scripts/x86c.py (10-22) : registres xmm en 4 mots de 32 bits, chaque
// opération flottante est une opération IEEE par voie, contraction FMA coupée, donc bit à bit = SSE.
// Mémoire invitée : win32_mem_read32 / win32_mem_write32 (carte plate, barrière d'écriture comprise).
// À inclure après la définition de State, win32_mem_read32 et win32_mem_write32.
#pragma once
#include <cstdint>
#include <cstring>

namespace x86c {
struct X { uint32_t u[4]; };
inline float f_(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t u_(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline uint32_t rd32(uint32_t a) { return win32_mem_read32(a); }
inline void wr32(uint32_t a, uint32_t v) { win32_mem_write32(a, v); }
inline X ldss(uint32_t a) { return {{rd32(a), 0u, 0u, 0u}}; }
inline X ldps(uint32_t a) { return {{rd32(a), rd32(a + 4u), rd32(a + 8u), rd32(a + 12u)}}; }
inline void stps(uint32_t a, X x) { for (int i = 0; i < 4; i++) wr32(a + 4u * i, x.u[i]); }
inline X x_get(const State* s, int k) { X x; std::memcpy(&x, &s->vec[k], 16); return x; }
inline void x_set(State* s, int k, X x) { std::memcpy(&s->vec[k], &x, 16); }

#define X86C_LANES(name, expr)                                            \
    inline X x_##name(X a, X b) {                                         \
        _Pragma("clang fp contract(off)") X r;                            \
        for (int i = 0; i < 4; i++) { const float p = f_(a.u[i]), q = f_(b.u[i]); r.u[i] = (expr); } \
        return r;                                                         \
    }
X86C_LANES(add, u_(p + q))
X86C_LANES(sub, u_(p - q))
X86C_LANES(mul, u_(p * q))
X86C_LANES(div, u_(p / q))
X86C_LANES(min, p < q ? a.u[i] : b.u[i])   // MINPS : la source si l'un est NaN ou si égaux
X86C_LANES(max, p > q ? a.u[i] : b.u[i])
#undef X86C_LANES
inline X x_and(X a, X b) { return {{a.u[0] & b.u[0], a.u[1] & b.u[1], a.u[2] & b.u[2], a.u[3] & b.u[3]}}; }
inline X x_or(X a, X b) { return {{a.u[0] | b.u[0], a.u[1] | b.u[1], a.u[2] | b.u[2], a.u[3] | b.u[3]}}; }
inline X x_xor(X a, X b) { return {{a.u[0] ^ b.u[0], a.u[1] ^ b.u[1], a.u[2] ^ b.u[2], a.u[3] ^ b.u[3]}}; }
inline X x_andn(X a, X b) { return {{~a.u[0] & b.u[0], ~a.u[1] & b.u[1], ~a.u[2] & b.u[2], ~a.u[3] & b.u[3]}}; }
inline X x_shuf(X a, X b, int m) { return {{a.u[m & 3], a.u[(m >> 2) & 3], b.u[(m >> 4) & 3], b.u[(m >> 6) & 3]}}; }
inline X x_unpcklps(X a, X b) { return {{a.u[0], b.u[0], a.u[1], b.u[1]}}; }
inline X x_unpckhps(X a, X b) { return {{a.u[2], b.u[2], a.u[3], b.u[3]}}; }
inline X x_movlhps(X a, X b) { return {{a.u[0], a.u[1], b.u[0], b.u[1]}}; }
inline X x_movhlps(X a, X b) { return {{b.u[2], b.u[3], a.u[2], a.u[3]}}; }
}  // namespace x86c
