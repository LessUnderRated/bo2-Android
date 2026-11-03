// cull_native.cpp — P911 : le tri de visibilité boîte contre cellules à 5 plans du multi (t6mp sub_762e30), natif.
// Tâche jq feuille (aucun appel) ; P910 : 18,3 % des ouvriers jq sur Rush contre 6,9 % sur Raid, le fil principal
// l'attend. Mêmes opérations, même ordre que le désassemblage (P910) :
//   b0 = [ecx..+0x10] & M, b1 = [ecx+0xc..+0x1c] & M (M = [MASQUE]) ; C = [SEUIL] ;
//   par enregistrement de 0x50 octets (5 vec4 R0..R4) : Mk = C < Rk (par voie),
//   passe A : v = Mk ? b1 : b0, m = v·Rk, sk = ((m1+m3)+(m0+m2)) − Rk.w ; t = max(s0, max(s1, max(s2, max(s3, s4))))
//     (maxps(a,b) = a>b ? a : b) ; une voie t < C → [compte] = 0, retour 0 ;
//   passe B : v = Mk ? b0 : b1, même calcul ; une voie t < C → gardé, sinon retiré : compte−−, échange avec le
//     dernier, même indice retesté. Retour normal : eax = [esp+8] (le tableau), edx aussi.
// Conventions : ecx = boîte, [esp+4] = &compte, [esp+8] = tableau ; `ret` simple, l'appelant dépile (seul appelant
// 0x763348 : recharge eax/ecx/edx/xmm après l'appel). Aucune FMA : une opération par instruction.
// BO2_CULL_NATIF : 0 = code levé ; 1 (défaut depuis P911 : Rush 22,2 → 37,9 img/s, 0 écart, 0 repli) = natif quand la plage écrite est sur la voie rapide d'écriture (sinon
// code levé) ; 2 = différentiel : natif sur une copie, code levé, comparaison du compte, du tableau et du retour.
// Témoins : [CULL-NATIF] (mode, puis appels/retraits/replis/écarts aux puissances de deux), [CULL-DIFF].
#pragma clang fp contract(off)
#include "remill/Arch/X86/Runtime/State.h"
#include "bo2_getenv.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" uint8_t* bo2_g_base;
extern "C" uint8_t* bo2_w32_slow_map;
extern "C" uint32_t bo2_w32_cold, bo2_w64_cold;
// Zombies (P914) : mêmes octets en t6zm 0x75fe50 aux adresses des constantes près (seul appelant 0x760368).
#if defined(BO2_TARGET_MPS)
extern "C" void* sub_459800_impl(State*, uint32_t, void*);   // P1053 : corps identiques, adresses seules
#define CULL_IMPL sub_459800_impl
#define CULL_ENTREE bo2_cull_mp
#define CULL_MASQUE 0x2931a20u
#define CULL_SEUIL 0xb913e0u
#elif defined(BO2_TARGET_ZMS)
extern "C" void* sub_4584c0_impl(State*, uint32_t, void*);
#define CULL_IMPL sub_4584c0_impl
#define CULL_ENTREE bo2_cull_zm
#define CULL_MASQUE 0x292faa0u
#define CULL_SEUIL 0xb8f3a0u
#else
extern "C" void* sub_75fe50_impl(State*, uint32_t, void*);
#define CULL_IMPL sub_75fe50_impl
#define CULL_ENTREE bo2_cull_zm
#define CULL_MASQUE 0x29e0120u
#define CULL_SEUIL 0xc786e0u
#endif

namespace {
inline uint32_t g32(uint32_t a) { uint32_t v; memcpy(&v, bo2_g_base + a, 4); return v; }
inline void lit4(uint32_t a, float o[4]) { memcpy(o, bo2_g_base + a, 16); }

// Une passe : s_k pour les 5 vec4, puis le max dans l'ordre du code ; vrai si une voie de t est sous C.
inline bool cull_passe(const float R[5][4], const bool Mk[5][4], const float vsi[4], const float vnon[4],
                       const float C[4]) {
    float s[5];
    for (int k = 0; k < 5; ++k) {
        float m[4];
        for (int j = 0; j < 4; ++j) { const float v = Mk[k][j] ? vsi[j] : vnon[j]; m[j] = v * R[k][j]; }
        const float a = m[1] + m[3];
        const float b = m[0] + m[2];
        const float l2 = a + b;
        s[k] = l2 - R[k][3];
    }
    float t = s[3] > s[4] ? s[3] : s[4];
    t = s[2] > t ? s[2] : t;
    t = s[1] > t ? s[1] : t;
    t = s[0] > t ? s[0] : t;
    return t < C[0] || t < C[1] || t < C[2] || t < C[3];
}

// Le corps sur le compte et le tableau pointés (image invitée ou copie) ; la boîte est lue dans l'invité.
// Vrai = gardé (retour eax = tableau), faux = rejeté (compte mis à 0, retour 0).
bool cull_run(uint8_t* pcompte, uint8_t* tab, uint32_t box, uint64_t* retraits) {
    uint32_t mk[4], u0[4], u1[4];
    memcpy(mk, bo2_g_base + CULL_MASQUE, 16);
    memcpy(u0, bo2_g_base + box, 16);
    memcpy(u1, bo2_g_base + box + 0xc, 16);
    float b0[4], b1[4], C[4];
    for (int j = 0; j < 4; ++j) { u0[j] &= mk[j]; u1[j] &= mk[j]; }
    memcpy(b0, u0, 16); memcpy(b1, u1, 16);
    lit4(CULL_SEUIL, C);
    int32_t n; memcpy(&n, pcompte, 4);
    for (int32_t i = 0; i < n;) {
        uint8_t* rec = tab + 0x50u * (uint32_t)i;
        float R[5][4]; memcpy(R, rec, 0x50);
        bool Mk[5][4];
        for (int k = 0; k < 5; ++k) for (int j = 0; j < 4; ++j) Mk[k][j] = C[j] < R[k][j];
        if (cull_passe(R, Mk, b1, b0, C)) { const int32_t z = 0; memcpy(pcompte, &z, 4); return false; }
        if (cull_passe(R, Mk, b0, b1, C)) { ++i; continue; }
        --n; memcpy(pcompte, &n, 4);
        uint8_t* der = tab + 0x50u * (uint32_t)n;
        uint8_t tmp[0x50]; memcpy(tmp, der, 0x50); memcpy(der, rec, 0x50); memcpy(rec, tmp, 0x50);
        ++*retraits;
    }
    return true;
}

bool plage_rapide(uint32_t a, uint32_t len) {
    const uint32_t last = a + len - 1u;
    if (last < a) return false;
    for (uint32_t g = a >> 16; g <= last >> 16; ++g) if (bo2_w32_slow_map[g]) return false;
    return true;
}
int cull_mode_resolve() {
    const char* e = bo2_getenv("BO2_CULL_NATIF");
    const int m = e ? atoi(e) : 1;
    fprintf(stderr, "[CULL-NATIF] mode=%d (BO2_CULL_NATIF=%s) : %s\n", m, e ? e : "(defaut 1)",
            m == 1 ? "natif (repli code leve hors voie rapide)" : m == 2 ? "DIFFERENTIEL natif/leve" : "code leve");
    return m;
}
std::atomic<uint64_t> g_appels{0}, g_retraits{0}, g_replis{0}, g_ecarts{0};
}  // namespace

extern "C" __attribute__((used)) void* CULL_ENTREE(State* s, uint32_t pc, void* mem) {
    static const int mode = cull_mode_resolve();
    if (mode == 0) return CULL_IMPL(s, pc, mem);
    const uint32_t sp = s->gpr.rsp.dword, box = s->gpr.rcx.dword, pc_ = g32(sp + 4), tab = g32(sp + 8);
    const int32_t n0 = (int32_t)g32(pc_);
    const uint64_t c = g_appels.fetch_add(1) + 1;
    uint64_t ret = 0;
    void* r = mem;
    const uint32_t len = n0 > 0 ? 0x50u * (uint32_t)n0 : 0u;
    if (mode == 2) {
        // Copie locale (compte + tableau) : le code levé peut céder la main à une autre coroutine.
        std::vector<uint8_t> buf(4 + len);
        memcpy(buf.data(), bo2_g_base + pc_, 4);
        if (len) memcpy(buf.data() + 4, bo2_g_base + tab, len);
        const uint32_t nr = cull_run(buf.data(), buf.data() + 4, box, &ret) ? tab : 0;
        r = CULL_IMPL(s, pc, mem);
        const bool ok = s->gpr.rax.dword == nr && !memcmp(buf.data(), bo2_g_base + pc_, 4) &&
                        (!len || !memcmp(buf.data() + 4, bo2_g_base + tab, len));
        if (!ok && g_ecarts.fetch_add(1) < 16) {
            int32_t nn, nl; memcpy(&nn, buf.data(), 4); memcpy(&nl, bo2_g_base + pc_, 4);
            fprintf(stderr, "[CULL-DIFF] tab=0x%08x n0=%d leve: eax=0x%08x n=%d natif: eax=0x%08x n=%d\n", tab, n0,
                    s->gpr.rax.dword, nl, nr, nn);
        }
    } else if ((bo2_w32_cold | bo2_w64_cold) == 0 && plage_rapide(pc_, 4) && (!len || plage_rapide(tab, len))) {
        const uint32_t e = cull_run(bo2_g_base + pc_, bo2_g_base + tab, box, &ret) ? tab : 0;
        s->gpr.rax.dword = e;
        s->gpr.rdx.dword = tab;
        s->gpr.rsp.dword += 4;   // ret
    } else {
        g_replis.fetch_add(1);
        r = CULL_IMPL(s, pc, mem);
    }
    if (ret) g_retraits.fetch_add(ret);
    if ((c & (c - 1)) == 0 && c >= 1024)
        fprintf(stderr, "[CULL-NATIF] appels=%llu retraits=%llu replis=%llu ecarts=%llu\n", (unsigned long long)c,
                (unsigned long long)g_retraits.load(), (unsigned long long)g_replis.load(),
                (unsigned long long)g_ecarts.load());
    return r;
}
// ── P917 : le parcours de cellules qui appelle ce tri (t6mp sub_763310, récursif) et ses deux feuilles, natifs ──
// sub_763310(N, P, r, cnt, arr) cdecl, retour eax = r. N = nœud (boîte min N[0..2], max N[3..5] ; u16 +0x18 enfants,
// +0x1a n2, +0x1c i2, +0x1e n1 ; u32 +0x20 liste u16, +0x24 décalage des enfants, pas 0x28). P = plans (pas 0x14 :
// n.xyz, d, x ; compte u32 à +0x140, 16 au plus dans le cadre). visA/visB = [[[fs:0x2c]]+8] / +0x10 (octets).
//   cnt ≠ 0 → arr = sub_762e30(N, &cnt, arr) ; arr = 0 → retour.
//   chaque plan : coin « haut » (n ≥ 0 ? max : min) t ; 0 ≥ t → retour (dehors) ; coin « bas » t2 ; !(t2 ≥ 0) →
//     gardé (copié) ; aucun gardé et cnt = 0 → visA[liste1[*]] = 1, visB[liste2[*]] = 1, retour.
//   enfants → récursion (N + [N+0x24] + 0x28 k, plans gardés, 0, cnt, arr) ; sinon feuille :
//     liste1 : surface E = [G+0x54] + 0x24 id ; visA[id] = 0, sub_762bb0 (plans, E) = 0, sub_762c40 (cnt, arr, E) = 0
//       → visA[id] = 1 ; liste2 ([G+0x50] + 2 i2) : B = [G+0x58] + 0x50 id + 0x38 ; visB[id] = 0, coin haut de B
//       sous chaque plan gardé non ≤ 0, sub_762c40 (cnt, arr, B) = 0 → visB[id] = 1.
// sub_762bb0 = 1 si un plan a 0 ≥ t (coin haut de E). sub_762c40 = la passe A du tri sur tout le tableau (al = 1 si
// un enregistrement rejette la boîte). Comparaisons comiss : NaN = « en dessous ».
// BO2_PORTAIL_NATIF : 0 = code levé ; 1 (défaut) = natif si visA/visB (64 Kio), le tableau et la pile sont sur la voie
// rapide ; 2 = différentiel : natif à blanc (écritures retenues, tableau copié), code levé, comparaison des octets lus,
// du tableau, du compte et du retour. Témoins : [PORTAIL-NATIF], [PORTAIL-DIFF].
#if defined(BO2_TARGET_MPS)
extern "C" void* sub_459ce0_impl(State*, uint32_t, void*);
#define PORTAIL_IMPL sub_459ce0_impl
#define PORTAIL_MONDE 0x39aea1cu
#else
extern "C" void* sub_763310_impl(State*, uint32_t, void*);
#define PORTAIL_IMPL sub_763310_impl
#define PORTAIL_MONDE 0x3a8e79cu
#endif
#define PORTAIL_ENTREE bo2_portail_mp
#include <unordered_set>

namespace {
struct Plan { float n[3]; float d; uint32_t x; };
struct Plans { Plan p[16]; uint32_t n = 0; };
static_assert(sizeof(Plan) == 0x14, "plan invité");

inline uint16_t g16(uint32_t a) { uint16_t v; memcpy(&v, bo2_g_base + a, 2); return v; }
inline void boite(uint32_t a, float o[6]) { memcpy(o, bo2_g_base + a, 24); }
// Le coin choisi par le signe de la normale, dans l'ordre du code : x·nx + d, puis y·ny + t, puis z·nz + t.
inline float coin(const Plan& p, const float* si, const float* non) {
    float t = (p.n[0] >= 0.0f ? si[0] : non[0]) * p.n[0] + p.d;
    t = (p.n[1] >= 0.0f ? si[1] : non[1]) * p.n[1] + t;
    t = (p.n[2] >= 0.0f ? si[2] : non[2]) * p.n[2] + t;
    return t;
}
// sub_762c40 : vrai si un enregistrement rejette la boîte (passe A du tri).
bool occulte(const uint8_t* tab, int32_t n, uint32_t box) {
    if (n <= 0) return false;
    uint32_t mk[4], u0[4], u1[4];
    memcpy(mk, bo2_g_base + CULL_MASQUE, 16);
    memcpy(u0, bo2_g_base + box, 16);
    memcpy(u1, bo2_g_base + box + 0xc, 16);
    for (int j = 0; j < 4; ++j) { u0[j] &= mk[j]; u1[j] &= mk[j]; }
    float b0[4], b1[4], C[4];
    memcpy(b0, u0, 16); memcpy(b1, u1, 16);
    lit4(CULL_SEUIL, C);
    for (int32_t i = 0; i < n; ++i) {
        float R[5][4]; memcpy(R, tab + 0x50u * (uint32_t)i, 0x50);
        bool Mk[5][4];
        for (int k = 0; k < 5; ++k) for (int j = 0; j < 4; ++j) Mk[k][j] = C[j] < R[k][j];
        if (cull_passe(R, Mk, b1, b0, C)) return true;
    }
    return false;
}
struct Ctx {
    uint8_t* tab;                              // tableau d'occulteurs (invité, ou copie en différentiel)
    uint32_t visA, visB, surf, modl, liste2;
    std::unordered_set<uint32_t>* ecr = nullptr;   // différentiel : octets posés, retenus
    std::vector<uint32_t>* lus = nullptr;          // différentiel : octets lus
    uint64_t retraits = 0;
};
inline bool vis_lit(Ctx& c, uint32_t a) {
    if (c.lus) { c.lus->push_back(a); if (c.ecr->count(a)) return true; }
    return bo2_g_base[a] != 0;
}
inline void vis_pose(Ctx& c, uint32_t a) {
    if (c.ecr) { c.ecr->insert(a); c.lus->push_back(a); } else bo2_g_base[a] = 1;
}
// Un nœud ; *pcnt et *parr sont les arguments du cadre (le tri les modifie).
void parcours(Ctx& c, uint32_t N, const Plans& P, int32_t* pcnt, uint32_t* parr) {
    if (*pcnt != 0) {
        uint8_t cb[4]; memcpy(cb, pcnt, 4);
        *parr = cull_run(cb, c.tab, N, &c.retraits) ? *parr : 0;
        memcpy(pcnt, cb, 4);
    }
    if (*parr == 0) return;
    float B[6]; boite(N, B);
    Plans G;
    for (uint32_t i = 0; i < P.n; ++i) {
        const Plan& p = P.p[i];
        if (0.0f >= coin(p, B + 3, B)) return;
        if (!(coin(p, B, B + 3) >= 0.0f)) G.p[G.n++] = p;
    }
    const uint32_t n1 = g16(N + 0x1e), n2 = g16(N + 0x1a);
    const uint32_t l1 = g32(N + 0x20), l2 = c.liste2 + 2u * g16(N + 0x1c);
    if (G.n == 0 && *pcnt == 0) {
        for (uint32_t k = 0; k < n1; ++k) vis_pose(c, c.visA + g16(l1 + 2 * k));
        for (uint32_t k = 0; k < n2; ++k) vis_pose(c, c.visB + g16(l2 + 2 * k));
        return;
    }
    if (const uint32_t nc = g16(N + 0x18)) {
        const uint32_t e0 = N + g32(N + 0x24);
        for (uint32_t k = 0; k < nc; ++k) {
            int32_t cnt = *pcnt; uint32_t arr = *parr;
            parcours(c, e0 + 0x28u * k, G, &cnt, &arr);
        }
        return;
    }
    for (uint32_t k = 0; k < n1; ++k) {
        const uint32_t id = g16(l1 + 2 * k), E = c.surf + 0x24u * id;
        if (vis_lit(c, c.visA + id)) continue;
        float S[6]; boite(E, S);
        bool dehors = false;
        for (uint32_t i = 0; i < G.n && !dehors; ++i) dehors = 0.0f >= coin(G.p[i], S + 3, S);
        if (dehors || occulte(c.tab, *pcnt, E)) continue;
        vis_pose(c, c.visA + id);
    }
    for (uint32_t k = 0; k < n2; ++k) {
        const uint32_t id = g16(l2 + 2 * k), M = c.modl + 0x50u * id + 0x38u;
        if (vis_lit(c, c.visB + id)) continue;
        float S[6]; boite(M, S);
        bool dehors = false;
        for (uint32_t i = 0; i < G.n && !dehors; ++i) dehors = 0.0f >= coin(G.p[i], S + 3, S);
        if (dehors || occulte(c.tab, *pcnt, M)) continue;
        vis_pose(c, c.visB + id);
    }
}
int portail_mode_resolve() {
    const char* e = bo2_getenv("BO2_PORTAIL_NATIF");
    const int m = e ? atoi(e) : 1;
    fprintf(stderr, "[PORTAIL-NATIF] mode=%d (BO2_PORTAIL_NATIF=%s) : %s\n", m, e ? e : "(defaut 1)",
            m == 1 ? "natif (repli code leve hors voie rapide)" : m == 2 ? "DIFFERENTIEL natif/leve" : "code leve");
    return m;
}
std::atomic<uint64_t> p_appels{0}, p_replis{0}, p_ecarts{0}, p_retraits{0};
}  // namespace

extern "C" __attribute__((used)) void* PORTAIL_ENTREE(State* s, uint32_t pc, void* mem) {
    static const int mode = portail_mode_resolve();
    if (mode == 0) return PORTAIL_IMPL(s, pc, mem);
    const uint32_t sp = s->gpr.rsp.dword, N = g32(sp + 4), P = g32(sp + 8), r = g32(sp + 0xc);
    int32_t cnt = (int32_t)g32(sp + 0x10);
    uint32_t arr = g32(sp + 0x14);
    const uint32_t t0 = g32(g32(s->addr.fs_base.dword + 0x2c)), monde = g32(PORTAIL_MONDE);
    // Le code levé ne lit [monde+0x5x] qu'aux feuilles : rien lu tant que le monde n'est pas chargé.
    Ctx c{nullptr, g32(t0 + 8), g32(t0 + 0x10), monde ? g32(monde + 0x54) : 0u, monde ? g32(monde + 0x58) : 0u,
          monde ? g32(monde + 0x50) : 0u};
    Plans pl; pl.n = g32(P + 0x140);
    const uint32_t len = cnt > 0 ? 0x50u * (uint32_t)cnt : 0u;
    const uint64_t a = p_appels.fetch_add(1) + 1;
    void* res = mem;
    if (pl.n > 16 || cnt < 0 || (bo2_w32_cold | bo2_w64_cold) != 0 || !plage_rapide(sp, 0x18) ||
        !plage_rapide(c.visA, 0x10000) || !plage_rapide(c.visB, 0x10000) || (len && !plage_rapide(arr, len))) {
        p_replis.fetch_add(1);
        res = PORTAIL_IMPL(s, pc, mem);
    } else {
        memcpy(pl.p, bo2_g_base + P, 0x14u * pl.n);
        if (mode == 2) {
            std::vector<uint8_t> copie(bo2_g_base + arr, bo2_g_base + arr + len);
            std::unordered_set<uint32_t> ecr; std::vector<uint32_t> lus;
            c.tab = copie.data(); c.ecr = &ecr; c.lus = &lus;
            int32_t ncnt = cnt; uint32_t narr = arr;
            parcours(c, N, pl, &ncnt, &narr);
            std::vector<uint8_t> avant(lus.size());
            for (size_t i = 0; i < lus.size(); ++i) avant[i] = bo2_g_base[lus[i]];
            res = PORTAIL_IMPL(s, pc, mem);
            bool ok = s->gpr.rax.dword == r && !memcmp(copie.data(), bo2_g_base + arr, len) &&
                      (cnt == 0 || ((int32_t)g32(sp + 0x10) == ncnt && g32(sp + 0x14) == narr));
            // Écarts classés : manque = le natif pose, l'invité reste à 0 ; surplus = le natif ne pose pas, 0 → non-0 ;
            // efface = non-0 → 0 (seul un autre fil peut l'avoir fait : le code n'écrit que des 1).
            size_t mauvais = 0, manque = 0, surplus = 0, efface = 0;
            for (size_t i = 0; i < lus.size(); ++i) {
                const uint8_t v = bo2_g_base[lus[i]];
                const bool pose = ecr.count(lus[i]) != 0;
                if (v == (pose ? 1 : avant[i])) continue;
                ++mauvais;
                if (pose && v == 0) ++manque; else if (!pose && avant[i] == 0) ++surplus; else if (v == 0) ++efface;
            }
            if ((!ok || mauvais) && p_ecarts.fetch_add(1) < 16)
                fprintf(stderr, "[PORTAIL-DIFF] N=0x%08x cnt=%d leve: eax=0x%x cnt=%d arr=0x%x natif: cnt=%d arr=0x%x "
                        "octets=%zu faux=%zu (manque=%zu surplus=%zu efface=%zu) ecrits=%zu visA=0x%x visB=0x%x fil=%u\n",
                        N, cnt, s->gpr.rax.dword, (int32_t)g32(sp + 0x10), g32(sp + 0x14), ncnt, narr, lus.size(),
                        mauvais, manque, surplus, efface, ecr.size(), c.visA, c.visB, s->addr.fs_base.dword);
        } else {
            c.tab = bo2_g_base + arr;
            const int32_t cnt0 = cnt;
            parcours(c, N, pl, &cnt, &arr);
            if (cnt0 != 0) { memcpy(bo2_g_base + sp + 0x10, &cnt, 4); memcpy(bo2_g_base + sp + 0x14, &arr, 4); }
            s->gpr.rax.dword = r;
            s->gpr.rsp.dword += 4;   // ret (cdecl : l'appelant dépile)
        }
        if (c.retraits) p_retraits.fetch_add(c.retraits);
    }
    if ((a & (a - 1)) == 0 && a >= 1024)
        fprintf(stderr, "[PORTAIL-NATIF] appels=%llu retraits=%llu replis=%llu ecarts=%llu\n", (unsigned long long)a,
                (unsigned long long)p_retraits.load(), (unsigned long long)p_replis.load(),
                (unsigned long long)p_ecarts.load());
    return res;
}
// ── Sa racine, t6mp sub_763780(a1, drapeau) avec ecx = {u32 plans, u32 nombre} : N = [a1+0x1c] (0 → retour 0) ; plans
// copiés (rep movsd) ; drapeau ≠ 0 → sub_763310(N, plans, 1, 0, 0x343fbc0) ; sinon les [0x3441010] occulteurs de
// 0x343fbc0 sont copiés flottant par flottant en x87 (fld/fstp : un NaN signalant ressort silencieux) dans un tableau
// de pile, puis sub_763310(N, plans, 1, cnt, tableau). Retour eax = celui de sub_763310 (1). Le tableau est mort au
// retour : en natif il reste côté hôte, aucune écriture invitée hors visA/visB. Même réglage BO2_PORTAIL_NATIF.
#if defined(BO2_TARGET_MPS)
extern "C" void* sub_45a150_impl(State*, uint32_t, void*);
#define RACINE_IMPL sub_45a150_impl
#define RACINE_OCC 0x33486a0u
#define RACINE_NOCC 0x3349af0u
#else
extern "C" void* sub_763780_impl(State*, uint32_t, void*);
#define RACINE_IMPL sub_763780_impl
#define RACINE_OCC 0x343fbc0u
#define RACINE_NOCC 0x3441010u
#endif
#define RACINE_ENTREE bo2_portail_racine_mp
namespace {
std::atomic<uint64_t> r_appels{0}, r_replis{0}, r_ecarts{0};
inline uint32_t x87_silencieux(uint32_t w) {
    return ((w & 0x7f800000u) == 0x7f800000u && (w & 0x3fffffu) && !(w & 0x400000u)) ? (w | 0x400000u) : w;
}
}  // namespace

extern "C" __attribute__((used)) void* RACINE_ENTREE(State* s, uint32_t pc, void* mem) {
    static const int mode = portail_mode_resolve();
    if (mode == 0) return RACINE_IMPL(s, pc, mem);
    const uint32_t sp = s->gpr.rsp.dword, a1 = g32(sp + 4), drapeau = g32(sp + 8), pd = s->gpr.rcx.dword;
    const uint32_t N = g32(a1 + 0x1c);
    const uint64_t a = r_appels.fetch_add(1) + 1;
    void* res = mem;
    const uint32_t np = N ? g32(pd + 4) : 0;
    const int32_t n0 = drapeau ? 0 : (int32_t)g32(RACINE_NOCC);
    const uint32_t t0 = g32(g32(s->addr.fs_base.dword + 0x2c)), monde = g32(PORTAIL_MONDE);
    Ctx c{nullptr, g32(t0 + 8), g32(t0 + 0x10), monde ? g32(monde + 0x54) : 0u, monde ? g32(monde + 0x58) : 0u,
          monde ? g32(monde + 0x50) : 0u};
    if (N == 0 && mode == 1) {
        s->gpr.rax.dword = 0;
        s->gpr.rsp.dword += 4;
    } else if (N == 0 || np > 16 || n0 < 0 || n0 > 64 || (bo2_w32_cold | bo2_w64_cold) != 0 ||
               !plage_rapide(c.visA, 0x10000) || !plage_rapide(c.visB, 0x10000)) {
        r_replis.fetch_add(1);
        res = RACINE_IMPL(s, pc, mem);
    } else {
        Plans pl; pl.n = np;
        if (np) memcpy(pl.p, bo2_g_base + g32(pd), 0x14u * np);
        std::vector<uint8_t> occ(drapeau ? 0u : 0x50u * (uint32_t)n0);
        for (size_t i = 0; i < occ.size(); i += 4) {
            const uint32_t w = x87_silencieux(g32(RACINE_OCC + (uint32_t)i));
            memcpy(occ.data() + i, &w, 4);
        }
        // drapeau ≠ 0 : compte 0, le tableau n'est jamais lu ; sinon un tableau non nul (sa valeur n'est pas lue).
        int32_t cnt = n0; uint32_t arr = drapeau ? RACINE_OCC : 1u;
        c.tab = occ.data();
        if (mode == 2) {
            std::unordered_set<uint32_t> ecr; std::vector<uint32_t> lus;
            c.ecr = &ecr; c.lus = &lus;
            parcours(c, N, pl, &cnt, &arr);
            std::vector<uint8_t> avant(lus.size());
            for (size_t i = 0; i < lus.size(); ++i) avant[i] = bo2_g_base[lus[i]];
            res = RACINE_IMPL(s, pc, mem);
            size_t mauvais = 0, manque = 0;
            for (size_t i = 0; i < lus.size(); ++i) {
                const uint8_t v = bo2_g_base[lus[i]];
                const bool pose = ecr.count(lus[i]) != 0;
                if (v == (pose ? 1 : avant[i])) continue;
                ++mauvais; if (pose && v == 0) ++manque;
            }
            if ((s->gpr.rax.dword != 1 || mauvais) && r_ecarts.fetch_add(1) < 16)
                fprintf(stderr, "[PORTAIL-DIFF] racine N=0x%08x cnt=%d leve: eax=0x%x octets=%zu faux=%zu (manque=%zu) "
                        "ecrits=%zu\n", N, n0, s->gpr.rax.dword, lus.size(), mauvais, manque, ecr.size());
        } else {
            parcours(c, N, pl, &cnt, &arr);
            s->gpr.rax.dword = 1;
            s->gpr.rsp.dword += 4;   // ret
        }
        if (c.retraits) p_retraits.fetch_add(c.retraits);
    }
    if ((a & (a - 1)) == 0 && a >= 1024)
        fprintf(stderr, "[PORTAIL-RACINE] appels=%llu replis=%llu ecarts=%llu\n", (unsigned long long)a,
                (unsigned long long)r_replis.load(), (unsigned long long)r_ecarts.load());
    return res;
}
