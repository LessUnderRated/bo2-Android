// skin_native.cpp — P781 : le dépouillement CPU (skinning) des sommets 1, 2, 3 et 4 os, en NEON natif.
// sub_7060e0 (seul appelant, balayage E8) répartit les sommets par nombre d'os sur 0x707370 / 0x7077d0 /
// 0x707d50 / 0x708400 ; les trois dernières pèsent 12,9 % du CPU tous fils (P679c) et ≈1,6 cœur en combat sur
// angola, la première ≈0,3 cœur. Ce
// sont les mêmes opérations, dans le même ordre, que le désassemblage (P781) :
//   R_k = somme pondérée des lignes k des matrices d'os (poids u16 · [0xbaeb50], dernier poids = 1 − les autres) ;
//   position = (x·R0 + y·R1) + (z·R2 + R3), w recopié de la source ; 8 octets recopiés tels quels ;
//   normale et tangente 10:10:10 : décompressées, (a·R0 + b·R1) + c·R2, bornées par maxps/minps, recompressées.
// Toutes les constantes sont relues dans l'image invitée. Aucune FMA : une intrinsèque par opération, jamais
// d'expression a*b+c que clang pourrait contracter.
// Conventions (pile à l'entrée, [esp] = retour, `ret` simple) :
//   1 os : ecx=src edx=dst  [esp+4]=mélange [esp+8]=n [esp+0xc]=matrices (pas d'indice de début)
//   2 os : eax=début ecx=src  [esp+4]=mélange [esp+8]=n [esp+0xc]=matrices [esp+0x10]=dst
//   3 os : eax=début ecx=dst edi=matrices  [esp+4]=src [esp+8]=mélange [esp+0xc]=n
//   4 os : ecx=début edx=dst esi=matrices  [esp+4]=src [esp+8]=mélange [esp+0xc]=n
// L'appelant recharge eax/ecx/edx/xmm/mm après chaque appel ; ebx/esi/edi/ebp ne sont pas touchés ici.
// BO2_SKIN_NATIVE : 0 = code levé ; 1 (défaut depuis P781 : img/s ×1,70, 0 écart, 0 repli) = natif quand toute
// la plage dst est sur la voie rapide d'écriture (sinon code levé) ; 2 = différentiel : natif dans un tampon,
// code levé, comparaison octet à octet.
// Témoins : [SKIN-NATIF] (mode, puis appels/sommets/replis aux puissances de deux), [SKIN-DIFF] (écarts).
#include "remill/Arch/X86/Runtime/State.h"
#include "bo2_getenv.h"
#include <arm_neon.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
// Exportés par win32_runtime.cpp : la carte plate, la carte des tranches lentes et les deux mots froids que teste
// la voie rapide d'écriture du code levé.
extern "C" uint8_t* bo2_g_base;
extern "C" uint8_t* bo2_w32_slow_map;
extern "C" uint32_t bo2_w32_cold, bo2_w64_cold;
// Zombies (ancienne version) : les quatre corps (0x780a00 / 0x780e60 / 0x7813e0 / 0x781a90) et leur seul appelant
// (0x77f770) sont ceux de la campagne aux adresses près ; SKIN_VA choisit.
// Multi (ancienne version, P915) : mêmes corps en 0x7824d0 / 0x782930 / 0x782eb0 / 0x783560 (0 octet d'écart hors des
// 9 constantes), appelant 0x7812a0 qui recharge aussi eax/ecx/edx après chaque appel.
// Steam (P1053) : mêmes corps, instruction par instruction ; seules les adresses changent (trois dernières colonnes).
#if defined(BO2_TARGET_SPS)
#define SKIN_VA(sp, zm, mp, ss, zs, ms) ss
#define sub_707370_impl sub_47b080_impl
#define sub_7077d0_impl sub_47b4e0_impl
#define sub_707d50_impl sub_47ba60_impl
#define sub_708400_impl sub_47c110_impl
#elif defined(BO2_TARGET_ZMS)
#define SKIN_VA(sp, zm, mp, ss, zs, ms) zs
#define sub_707370_impl sub_47acf0_impl
#define sub_7077d0_impl sub_47b150_impl
#define sub_707d50_impl sub_47b6d0_impl
#define sub_708400_impl sub_47bd80_impl
#else
#define SKIN_VA(sp, zm, mp, ss, zs, ms) ms
#define sub_707370_impl sub_47ab10_impl
#define sub_7077d0_impl sub_47af70_impl
#define sub_707d50_impl sub_47b4f0_impl
#define sub_708400_impl sub_47bba0_impl
#endif
extern "C" void* sub_707370_impl(State*, uint32_t, void*);
extern "C" void* sub_7077d0_impl(State*, uint32_t, void*);
extern "C" void* sub_707d50_impl(State*, uint32_t, void*);
extern "C" void* sub_708400_impl(State*, uint32_t, void*);
namespace {
struct SkinK { int32x4_t mask, xr; float32x4_t bias, uscale, lo, hi, pscale, wscale, one; };
inline float32x4_t skin_q(uint32_t a) { float32x4_t v; memcpy(&v, bo2_g_base + a, 16); return v; }
inline uint32_t skin_u32(uint32_t a) { uint32_t v; memcpy(&v, bo2_g_base + a, 4); return v; }
inline uint32_t skin_u16(uint32_t a) { uint16_t v; memcpy(&v, bo2_g_base + a, 2); return v; }
SkinK skin_consts() {
    return {vreinterpretq_s32_f32(skin_q(SKIN_VA(0xbaeb40, 0xd1b350, 0xd27330, 0xb14b30, 0xc5dee0, 0xc61970))),
            vreinterpretq_s32_f32(skin_q(SKIN_VA(0xa98980, 0xc0d9a0, 0xbf7a40, 0xb198d0, 0xc62c90, 0xc66720))), skin_q(SKIN_VA(0xbaeb30, 0xd1b340, 0xd27320, 0xb14b20, 0xc5ded0, 0xc61960)),
            skin_q(SKIN_VA(0xbaeb00, 0xd1b310, 0xd272f0, 0xb14af0, 0xc5dea0, 0xc61930)), skin_q(SKIN_VA(0xbaeb10, 0xd1b320, 0xd27300, 0xb14b00, 0xc5deb0, 0xc61940)),
            skin_q(SKIN_VA(0xaaf6b0, 0xc4c2f0, 0xc09c50, 0xb197f0, 0xc62bc0, 0xc66640)), skin_q(SKIN_VA(0xbaeb20, 0xd1b330, 0xd27310, 0xb14b10, 0xc5dec0, 0xc61950)),
            skin_q(SKIN_VA(0xbaeb50, 0xd1b360, 0xd27340, 0xb14b40, 0xc5def0, 0xc61980)), skin_q(SKIN_VA(0xbb9c80, 0xd26540, 0xc89e60, 0xa8b790, 0xbab990, 0xc71da0))};
}
inline uint32_t skin_pack(uint32_t d, const float32x4_t* R, const SkinK& k) {
    const int32x4_t i = veorq_s32(vandq_s32(vdupq_n_s32((int32_t)d), k.mask), k.xr);
    const float32x4_t u = vmulq_f32(vaddq_f32(vcvtq_f32_s32(i), k.bias), k.uscale);
    float32x4_t v = vaddq_f32(vaddq_f32(vmulq_laneq_f32(R[0], u, 0), vmulq_laneq_f32(R[1], u, 1)),
                              vmulq_laneq_f32(R[2], u, 2));
    v = vbslq_f32(vcgtq_f32(v, k.lo), v, k.lo);   // maxps v, lo : v > lo ? v : lo (NaN -> lo)
    v = vbslq_f32(vcltq_f32(v, k.hi), v, k.hi);   // minps v, hi : v < hi ? v : hi
    const int32x4_t r = vandq_s32(vcvtnq_s32_f32(vmulq_f32(v, k.pscale)), k.mask);   // cvtps2dq, au plus près
    return (uint32_t)(vgetq_lane_s32(r, 0) | vgetq_lane_s32(r, 1) | vgetq_lane_s32(r, 2) | vgetq_lane_s32(r, 3));
}
inline float32x4_t skin_w(uint32_t a, const SkinK& k) { return vmulq_f32(vdupq_n_f32((float)skin_u16(a)), k.wscale); }
// Enregistrement de mélange : 2·B−1 u16, os = décalage en octets dans `mats`, poids en u16.
template <int B> inline void skin_blend(uint32_t rec, uint32_t mats, const SkinK& k, float32x4_t* R) {
    auto row = [&](int i, int r) { return skin_q(mats + skin_u16(rec + 2 * i) + 16u * r); };
    if (B == 1) {
        for (int r = 0; r < 4; ++r) R[r] = row(0, r);
    } else if (B == 2) {
        const float32x4_t w = skin_w(rec + 4, k), w0 = vsubq_f32(k.one, w);
        for (int r = 0; r < 4; ++r) R[r] = vaddq_f32(vmulq_f32(row(0, r), w0), vmulq_f32(row(1, r), w));
    } else if (B == 3) {
        const float32x4_t wa = skin_w(rec + 8, k), wb = skin_w(rec + 4, k);
        const float32x4_t wc = vsubq_f32(vsubq_f32(k.one, wa), wb);
        for (int r = 0; r < 4; ++r)
            R[r] = vaddq_f32(vmulq_f32(row(0, r), wc), vaddq_f32(vmulq_f32(row(3, r), wa), vmulq_f32(row(1, r), wb)));
    } else {
        const float32x4_t w0 = skin_w(rec + 12, k), w1 = skin_w(rec + 8, k), w2 = skin_w(rec + 4, k);
        const float32x4_t wd = vsubq_f32(vsubq_f32(vsubq_f32(k.one, w0), w1), w2);
        for (int r = 0; r < 4; ++r)
            R[r] = vaddq_f32(vmulq_f32(row(0, r), wd),
                             vaddq_f32(vmulq_f32(row(1, r), w2),
                                       vaddq_f32(vmulq_f32(row(5, r), w0), vmulq_f32(row(3, r), w1))));
    }
}
template <int B> void skin_run(uint32_t src, uint32_t rec, uint32_t mats, int32_t n, uint8_t* out) {
    const SkinK k = skin_consts();
    float32x4_t R[4];
    for (int32_t i = 0; i < n; ++i, src += 32, rec += 2 * (2 * B - 1), out += 32) {
        skin_blend<B>(rec, mats, k, R);
        const float32x4_t p = skin_q(src);
        float32x4_t o = vaddq_f32(vaddq_f32(vmulq_laneq_f32(R[0], p, 0), vmulq_laneq_f32(R[1], p, 1)),
                                  vaddq_f32(vmulq_laneq_f32(R[2], p, 2), R[3]));
        o = vcopyq_laneq_f32(o, 3, p, 3);
        const uint32_t nt[2] = {skin_pack(skin_u32(src + 0x18), R, k), skin_pack(skin_u32(src + 0x1c), R, k)};
        uint8_t mid[8]; memcpy(mid, bo2_g_base + src + 0x10, 8);   // toutes les lectures avant la première écriture
        memcpy(out, &o, 16); memcpy(out + 0x10, mid, 8); memcpy(out + 0x18, nt, 8);
    }
}
// La voie rapide d'écriture du code levé, sur toute la plage : rien d'armé, aucune tranche de 64 Kio marquée.
bool skin_dst_fast(uint32_t dst, int32_t n) {
    if ((bo2_w32_cold | bo2_w64_cold) != 0) return false;
    const uint32_t last = dst + (uint32_t)n * 32u - 1u;
    if (last < dst) return false;
    for (uint32_t g = dst >> 16; g <= last >> 16; ++g) if (bo2_w32_slow_map[g]) return false;
    return true;
}
int skin_mode_resolve() {
    const char* e = bo2_getenv("BO2_SKIN_NATIVE");
    const int m = e ? atoi(e) : 1;
    fprintf(stderr, "[SKIN-NATIF] mode=%d (BO2_SKIN_NATIVE=%s) : %s\n", m, e ? e : "(defaut 1)",
            m == 1 ? "natif (repli code leve hors voie rapide)" : m == 2 ? "DIFFERENTIEL natif/leve" : "code leve");
    return m;
}
struct SkinStat { std::atomic<uint64_t> appels{0}, sommets{0}, replis{0}, ecarts{0}; };
SkinStat g_skin[5];
void skin_diff(int B, uint32_t dst, int32_t n, const uint8_t* nat) {
    SkinStat& st = g_skin[B];
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t* g = bo2_g_base + dst + 32u * (uint32_t)i;
        if (!memcmp(g, nat + 32 * i, 32)) continue;
        if (st.ecarts.fetch_add(1) < 16) {
            uint32_t a[8], b[8]; memcpy(a, g, 32); memcpy(b, nat + 32 * i, 32);
            fprintf(stderr, "[SKIN-DIFF] os=%d sommet %d dst=0x%08x leve=%08x %08x %08x %08x %08x %08x %08x %08x "
                    "natif=%08x %08x %08x %08x %08x %08x %08x %08x\n", B, i, dst + 32u * (uint32_t)i,
                    a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        }
    }
}
template <int B> void* skin_entry(State* s, uint32_t pc, void* mem, void* (*impl)(State*, uint32_t, void*),
                                  uint32_t src, uint32_t dst, uint32_t rec, uint32_t mats, int32_t n) {
    static const int mode = skin_mode_resolve();
    if (mode == 0 || n <= 0) return impl(s, pc, mem);
    SkinStat& st = g_skin[B];
    const uint64_t c = st.appels.fetch_add(1) + 1;
    st.sommets.fetch_add((uint64_t)n);
    void* r = mem;
    if (mode == 2) {
        std::vector<uint8_t> buf((size_t)n * 32);   // local : le code levé peut céder la main à une autre coroutine
        skin_run<B>(src, rec, mats, n, buf.data());
        r = impl(s, pc, mem);
        skin_diff(B, dst, n, buf.data());
    } else if (skin_dst_fast(dst, n)) {
        skin_run<B>(src, rec, mats, n, bo2_g_base + dst);
        s->gpr.rsp.dword += 4;   // ret
    } else {
        st.replis.fetch_add(1);
        r = impl(s, pc, mem);
    }
    if ((c & (c - 1)) == 0 && c >= 1024)
        fprintf(stderr, "[SKIN-NATIF] os=%d appels=%llu sommets=%llu replis=%llu ecarts=%llu\n", B,
                (unsigned long long)c, (unsigned long long)st.sommets.load(), (unsigned long long)st.replis.load(),
                (unsigned long long)st.ecarts.load());
    return r;
}
}  // namespace
extern "C" __attribute__((used)) void* bo2_skin1(State* s, uint32_t pc, void* mem) {
    const uint32_t sp = s->gpr.rsp.dword;
    return skin_entry<1>(s, pc, mem, sub_707370_impl, s->gpr.rcx.dword, s->gpr.rdx.dword, skin_u32(sp + 4),
                         skin_u32(sp + 0xc), (int32_t)skin_u32(sp + 8));
}
extern "C" __attribute__((used)) void* bo2_skin2(State* s, uint32_t pc, void* mem) {
    const uint32_t sp = s->gpr.rsp.dword, off = s->gpr.rax.dword << 5;
    return skin_entry<2>(s, pc, mem, sub_7077d0_impl, s->gpr.rcx.dword + off, skin_u32(sp + 0x10) + off,
                         skin_u32(sp + 4), skin_u32(sp + 0xc), (int32_t)skin_u32(sp + 8));
}
extern "C" __attribute__((used)) void* bo2_skin3(State* s, uint32_t pc, void* mem) {
    const uint32_t sp = s->gpr.rsp.dword, off = s->gpr.rax.dword << 5;
    return skin_entry<3>(s, pc, mem, sub_707d50_impl, skin_u32(sp + 4) + off, s->gpr.rcx.dword + off,
                         skin_u32(sp + 8), s->gpr.rdi.dword, (int32_t)skin_u32(sp + 0xc));
}
extern "C" __attribute__((used)) void* bo2_skin4(State* s, uint32_t pc, void* mem) {
    const uint32_t sp = s->gpr.rsp.dword, off = s->gpr.rcx.dword << 5;
    return skin_entry<4>(s, pc, mem, sub_708400_impl, skin_u32(sp + 4) + off, s->gpr.rdx.dword + off,
                         skin_u32(sp + 8), s->gpr.rsi.dword, (int32_t)skin_u32(sp + 0xc));
}
