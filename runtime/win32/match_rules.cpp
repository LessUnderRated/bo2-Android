// match_rules.cpp — P1057 : `_globallogic.gsc`, absent de toutes les zones Steam (servi jadis par la zone du jour),
// remplacé par NOTRE script (runtime/match_rules/, compilé par gsc-tool), servi comme ScriptParseTree {nom, taille, tampon}
// quand le jeu le demande à DB_FindXAssetHeader (type 48). BO2_MATCH_RULES=<fichier> : compilé hôte, pour essayer sans
// reconstruire ; BO2_MATCH_RULES=0 : désarmé ; sans la variable : embarqué (mp.inc, zm.inc), sauf si la zone d'origine est dans zone/all. Témoin : [REGLES-GSC].
#include "remill/Arch/X86/Runtime/State.h"
#include "bo2_getenv.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C" const char* win32_game_dir();
extern "C" uint32_t win32_guest_alloc(uint32_t size, uint32_t align);
extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" uint8_t win32_mem_read8(uint32_t va);
extern "C" void win32_mem_write8(uint32_t va, uint8_t v);
extern "C" void win32_mem_write32(uint32_t va, uint32_t v);

#if defined(BO2_TARGET_MPS)
extern "C" void* sub_59a120_impl(State*, uint32_t, void*);
#define FIND_IMPL sub_59a120_impl
static const char kNom[] = "maps/mp/gametypes/_globallogic.gsc";
#else
extern "C" void* sub_59b6c0_impl(State*, uint32_t, void*);
#define FIND_IMPL sub_59b6c0_impl
static const char kNom[] = "maps/mp/gametypes_zm/_globallogic.gsc";
#endif

static const uint8_t kBlob[] = {
#if defined(BO2_TARGET_MPS)
#include "../match_rules/mp.inc"
#else
#include "../match_rules/zm.inc"
#endif
};

// Le fichier d'origine dans zone/all : c'est lui que le jeu charge, on ne sert rien.
static bool zone_presente() {
#if defined(BO2_TARGET_MPS)
    const std::string f = std::string(win32_game_dir()) + "/zone/all/ffotd_tu17_mp_147.ff";
#else
    const std::string f = std::string(win32_game_dir()) + "/zone/all/ffotd_tu17_zm_147.ff";
#endif
    FILE* p = fopen(f.c_str(), "rb");
    if (p) fclose(p);
    return p != nullptr;
}

static uint32_t entete() {
    static uint32_t h = [] {
        const char* e = bo2_getenv("BO2_MATCH_RULES");
        if ((e && !strcmp(e, "0")) || (!e && zone_presente())) { fprintf(stderr, "[REGLES-GSC] desarme\n"); return 0u; }
        std::vector<uint8_t> d(kBlob, kBlob + sizeof kBlob);
        if (e) {
            d.clear();
            if (FILE* f = fopen(e, "rb")) {
                fseek(f, 0, SEEK_END); d.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
                d.resize(fread(d.data(), 1, d.size(), f)); fclose(f);
            }
            if (d.empty()) { fprintf(stderr, "[REGLES-GSC] REFUS : %s illisible\n", e); return 0u; }
        }
        const uint32_t buf = win32_guest_alloc((uint32_t)d.size() + 1, 16);
        for (size_t i = 0; i < d.size(); i++) win32_mem_write8(buf + (uint32_t)i, d[i]);
        const uint32_t nm = win32_guest_alloc(sizeof kNom, 4);
        for (size_t i = 0; i < sizeof kNom; i++) win32_mem_write8(nm + (uint32_t)i, (uint8_t)kNom[i]);
        const uint32_t t = win32_guest_alloc(12, 4);
        win32_mem_write32(t, nm); win32_mem_write32(t + 4, (uint32_t)d.size()); win32_mem_write32(t + 8, buf);
        fprintf(stderr, "[REGLES-GSC] %s : %zu octets, entete 0x%08x\n", e ? e : "embarque", d.size(), t);
        return t;
    }();
    return h;
}

// Tampons de script vus passer : une erreur de VM se situe en script+fonction ([GSC-ERR]).
static std::vector<std::pair<uint32_t, uint32_t>> g_blobs;

static void lit_chaine(uint32_t p, char* o, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) { o[i] = (char)win32_mem_read8(p + (uint32_t)i); if (!o[i]) break; }
    o[i] = 0;
}

extern "C" bool bo2_regles_situe(uint32_t v) {
    for (const auto& [buf, len] : g_blobs) {
        if (v < buf || v >= buf + len) continue;
        const uint32_t off = v - buf, exports = win32_mem_read32(buf + 0x1c);
        const uint32_t n = win32_mem_read32(buf + 0x34) & 0xffff;
        uint32_t best = 0, nm = 0;
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t e = buf + exports + 12 * k, o = win32_mem_read32(e + 4);
            if (o <= off && o >= best) { best = o; nm = win32_mem_read32(e + 8) & 0xffff; }
        }
        if (!best) return false;   // en-tete ou table : pas du code
        char f[64], sc[96];
        lit_chaine(buf + nm, f, sizeof f);
        lit_chaine(buf + (win32_mem_read32(buf + 0x30) & 0xffff), sc, sizeof sc);
        fprintf(stderr, "[GSC-ERR] %s %s+0x%x\n", sc, f, off - best);
        return true;
    }
    return false;
}

extern "C" __attribute__((used)) void* bo2_regles_find(State* s, uint32_t pc, void* mem) {
    const uint32_t esp = s->gpr.rsp.dword;
    if (win32_mem_read32(esp + 4) == 48u) {
        const uint32_t p = win32_mem_read32(esp + 8);
        size_t i = 0;
        while (kNom[i] && win32_mem_read8(p + (uint32_t)i) == (uint8_t)kNom[i]) i++;
        if (!kNom[i] && !win32_mem_read8(p + (uint32_t)i)) {
            if (const uint32_t h = entete()) {
                static int n = 0;
                if (n++ < 3) fprintf(stderr, "[REGLES-GSC] servi (%d)\n", n);
                s->gpr.rax.dword = h;
                if (g_blobs.empty() || g_blobs[0].first != win32_mem_read32(h + 8)) g_blobs.insert(g_blobs.begin(), {win32_mem_read32(h + 8), win32_mem_read32(h + 4)});
                s->gpr.rsp.dword = esp + 4;
                return mem;
            }
        }
    }
    const bool script = win32_mem_read32(esp + 4) == 48u;
    void* r = FIND_IMPL(s, pc, mem);
    if (script && s->gpr.rax.dword) {
        const uint32_t h = s->gpr.rax.dword, buf = win32_mem_read32(h + 8);
        if (buf) {
            bool vu = false;
            for (const auto& b : g_blobs) vu = vu || b.first == buf;
            if (!vu) g_blobs.push_back({buf, win32_mem_read32(h + 4)});
        }
    }
    return r;
}
