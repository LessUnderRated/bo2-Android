// heap_code.cpp — EXECUTER LE CODE X86 QUE LE JEU ECRIT DANS LE TAS (P708).
//
// Une fonction du jeu peut ecrire du code dans un bloc HeapAlloc et y sauter : le
// `ret` d'un talon part alors dans le tas, le code du tas appelle des fonctions de
// .text, puis `jmp` vers une fonction de .text qui rend a l'appelant d'origine.
//
// Rien de ce code n'est dans le lift : il n'existe qu'a l'execution. Sans ceci,
// __remill_function_return ignorait la cible et l'appelant lifte reprenait apres son
// `call` avec un mot de trop sur la pile (P707).
//
// Ici, Unicorn execute le code du tas sur la carte PLATE invitee (bo2_g_base, meme
// memoire que le code lifte). Chaque sortie vers l'image ou une sentinelle est rendue au dispatch :
//   - le code du tas a fait `call` vers .text ou un import : on appelle la cible
//     comme un appel invite (le retour est deja empile), puis on reprend dans le tas ;
//   - il a fait `jmp` : meme chose ; si la cible rend hors du tas, c'est la suite ;
//   - il a fait `ret` hors du tas : c'est la suite, rendue a l'appelant.
// L'appelant compare la suite au retour qu'il attend ; si elle differe (`push X ; ret`),
// il reprend a la racine du fil (bo2_reprise_non_locale).
// Pas de x87 transfere (compte dans [HEAP-CODE] x87= ; a faire si non nul).
#include "remill/Arch/X86/Runtime/State.h"
#include <unicorn/unicorn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" uint8_t* bo2_g_base;
extern "C" int win32_current_tid();
extern "C" void* bo2_heap_code_appel(State& state, uint32_t addr, void* mem);   // win32_runtime.cpp

namespace {

// t6sp : le code ecrit a l'execution n'est que dans le tas invite (GUEST_HEAP_BASE,
// kernel32_impl.cpp). En dessous : l'image t6sp et les retours marques 0x10xxxxxx (P716).
constexpr uint32_t kBas       = 0x20000000u;
constexpr uint32_t kImage     = 0x400000u;      // image (SizeOfImage <= 0x3fbc000) : .text lifte,
constexpr uint32_t kImageFin  = 0x43bc000u;     //   fentes d'IAT = cibles d'import du dispatch
constexpr uint32_t kHaut      = 0xF0000000u;    // GUEST_HEAP_END, kernel32_impl.cpp
constexpr uint32_t kGdt       = 0x7FF00000u;    // trou TEB (0x7FF00000..0x80000000) : page jamais servie
constexpr uint64_t kEspace    = 1ull << 32;
constexpr uint64_t kMaxInstr  = 200'000'000ull;
constexpr int      kSelDonnees = 2, kSelFs = 3;

struct Arret {
    uint32_t sortie = 0;        // premiere adresse qui n'est pas du code du tas
    uint32_t der = 0, der_taille = 0;   // derniere instruction executee dans le tas
    uint64_t n = 0, x87 = 0;
    int intr = -1;
    bool invalide = false;
};

struct Moteur {
    uc_engine* uc = nullptr;
    alignas(4096) uint8_t gdt[4096];
    Arret a;
};

uint64_t descripteur(uint32_t base, uint32_t limite, uint32_t acces, uint32_t drap) {
    return (limite & 0xffffull) | ((uint64_t)(base & 0xffffffu) << 16) | ((uint64_t)acces << 40) |
           ((uint64_t)((limite >> 16) & 0xfu) << 48) | ((uint64_t)drap << 52) |
           ((uint64_t)((base >> 24) & 0xffu) << 56);
}

}  // namespace
extern "C" bool bo2_est_heap_code(uint32_t va);
namespace {

void sur_code(uc_engine* uc, uint64_t addr, uint32_t taille, void* u) {
    Arret& a = *static_cast<Arret*>(u);
    const uint32_t va = (uint32_t)addr;
    if (!bo2_est_heap_code(va)) { a.sortie = va; uc_emu_stop(uc); return; }
    a.der = va; a.der_taille = taille;
    uint8_t op = bo2_g_base[va];
    for (int i = 0; i < 4 && (op == 0x66 || op == 0xF2 || op == 0xF3 || op == 0x9B); i++) op = bo2_g_base[va + i + 1];
    if (op >= 0xD8 && op <= 0xDF) a.x87++;
    if (++a.n > kMaxInstr) uc_emu_stop(uc);
}
void sur_intr(uc_engine* uc, uint32_t no, void* u) { static_cast<Arret*>(u)->intr = (int)no; uc_emu_stop(uc); }
bool sur_invalide(uc_engine* uc, void* u) { static_cast<Arret*>(u)->invalide = true; uc_emu_stop(uc); return false; }

void verifie(uc_err e, const char* quoi) {
    if (e == UC_ERR_OK) return;
    fprintf(stderr, "[HEAP-CODE] Unicorn : %s a echoue : %s\n", quoi, uc_strerror(e));
    abort();
}

Moteur& moteur() {
    thread_local Moteur* m = nullptr;
    if (m) return *m;
    m = new Moteur();
    verifie(uc_open(UC_ARCH_X86, UC_MODE_32, &m->uc), "uc_open");
    verifie(uc_mem_map_ptr(m->uc, 0, kGdt, UC_PROT_ALL, bo2_g_base), "carte basse");
    verifie(uc_mem_map_ptr(m->uc, kGdt, sizeof m->gdt, UC_PROT_ALL, m->gdt), "carte GDT");
    verifie(uc_mem_map_ptr(m->uc, kGdt + sizeof m->gdt, kEspace - kGdt - sizeof m->gdt, UC_PROT_ALL,
                           bo2_g_base + kGdt + sizeof m->gdt), "carte haute");
    const uint64_t plat = descripteur(0, 0xfffff, 0x93, 0xc);
    memcpy(m->gdt + 8 * kSelDonnees, &plat, 8);
    uc_x86_mmr gdtr{0, kGdt, 0x100, 0};
    verifie(uc_reg_write(m->uc, UC_X86_REG_GDTR, &gdtr), "GDTR");
    const uint32_t sel = kSelDonnees << 3;
    for (int r : {UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES}) verifie(uc_reg_write(m->uc, r, &sel), "selecteur");
    uc_hook h;
    verifie(uc_hook_add(m->uc, &h, UC_HOOK_CODE, (void*)sur_code, &m->a, 1, 0), "crochet code");
    verifie(uc_hook_add(m->uc, &h, UC_HOOK_INTR, (void*)sur_intr, &m->a, 1, 0), "crochet intr");
    verifie(uc_hook_add(m->uc, &h, UC_HOOK_INSN_INVALID, (void*)sur_invalide, &m->a, 1, 0), "crochet invalide");
    fprintf(stderr, "[HEAP-CODE] moteur Unicorn pret (tid %d)\n", win32_current_tid());
    return *m;
}

constexpr int kGpr[8] = {UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                         UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP};
uint32_t* gpr(State& s, int i) {
    Reg* r[8] = {&s.gpr.rax, &s.gpr.rbx, &s.gpr.rcx, &s.gpr.rdx, &s.gpr.rsi, &s.gpr.rdi, &s.gpr.rbp, &s.gpr.rsp};
    return &r[i]->dword;
}

void vers_uc(Moteur& m, State& s, uint32_t pc) {
    const uint64_t fs = descripteur(s.addr.fs_base.dword, 0xfff, 0x93, 0x4);
    memcpy(m.gdt + 8 * kSelFs, &fs, 8);
    const uint32_t sel = kSelFs << 3;
    verifie(uc_reg_write(m.uc, UC_X86_REG_FS, &sel), "FS");
    for (int i = 0; i < 8; i++) uc_reg_write(m.uc, kGpr[i], gpr(s, i));
    const uint32_t fl = 0x202u | (s.aflag.cf ? 1u : 0u) | (s.aflag.pf ? 4u : 0u) | (s.aflag.af ? 0x10u : 0u) |
                        (s.aflag.zf ? 0x40u : 0u) | (s.aflag.sf ? 0x80u : 0u) | (s.aflag.df ? 0x400u : 0u) |
                        (s.aflag.of ? 0x800u : 0u);
    uc_reg_write(m.uc, UC_X86_REG_EFLAGS, &fl);
    for (int i = 0; i < 8; i++) uc_reg_write(m.uc, UC_X86_REG_XMM0 + i, &s.vec[i].xmm);
    uc_reg_write(m.uc, UC_X86_REG_EIP, &pc);
}

void depuis_uc(Moteur& m, State& s) {
    for (int i = 0; i < 8; i++) uc_reg_read(m.uc, kGpr[i], gpr(s, i));
    uint32_t fl = 0;
    uc_reg_read(m.uc, UC_X86_REG_EFLAGS, &fl);
    s.aflag.cf = fl & 1u; s.aflag.pf = (fl >> 2) & 1u; s.aflag.af = (fl >> 4) & 1u; s.aflag.zf = (fl >> 6) & 1u;
    s.aflag.sf = (fl >> 7) & 1u; s.aflag.df = (fl >> 10) & 1u; s.aflag.of = (fl >> 11) & 1u;
    for (int i = 0; i < 8; i++) uc_reg_read(m.uc, UC_X86_REG_XMM0 + i, &s.vec[i].xmm);
}

uint32_t lit32(uint32_t va) { uint32_t v; memcpy(&v, bo2_g_base + va, 4); return v; }

// P729 : retours dans le tas qu'attendent les sessions en cours sur ce fil. Quand le code du tas
// appelle une fonction liftee, le `ret` de celle-ci vers le tas rend a CETTE session
// (bo2_heap_code_appel revient, la boucle reprend a s._0) : ce n'est pas une nouvelle entree.
// P728 : sans ceci, chaque retour ouvrait une session imbriquee (6 de suite) et la derniere
// comparait sa sortie a une case de pile etrangere -> [REPRISE] a tort, guest finished.
constexpr int kAttendusMax = 64;
thread_local uint32_t t_attendus[kAttendusMax];
thread_local int t_prof = 0;

}  // namespace

extern "C" bool bo2_heap_code_attend(uint32_t va) {
    return t_prof > 0 && t_attendus[t_prof - 1] == va;
}

// Vrai si `va` peut etre du code ecrit a l'execution : ni l'image (lift, fentes d'IAT),
// ni la page GDT privee, ni une sentinelle du runtime (retour de fil 0xDEAD0000,
// imports dynamiques 0xdead01xx, rappels 0xe5/0xe6/0xe7xxxxxx).
extern "C" bool bo2_est_heap_code(uint32_t va) {
    if (va < kBas || va >= kHaut || (va >= kImage && va < kImageFin)) return false;
    if (va - kGdt < 0x1000u) return false;
    if ((va & 0xffff0000u) == 0xdead0000u) return false;
    const uint32_t haut = va >> 24;
    return haut != 0xe5u && haut != 0xe6u && haut != 0xe7u && va != 0xdeaddeadu && va != 0xdeadbeefu;
}

// Execute le code du tas a partir de `pc` jusqu'a ce qu'un `ret` (direct, ou celui
// d'une fonction de .text rejointe par `jmp`) quitte le tas. Rend l'adresse de suite.
extern "C" uint32_t bo2_heap_code(State& s, uint32_t pc, void** mem) {
    Moteur& m = moteur();
    const uint32_t entree = pc, esp0 = s.gpr.rsp.dword;
    uint64_t total = 0, x87 = 0;
    int appels = 0;
    for (;;) {
        m.a = Arret{};
        vers_uc(m, s, pc);
        verifie(uc_ctl_flush_tb(m.uc), "vidage du cache de traduction");   // le tas est reecrit hors d'Unicorn
        const uc_err e = uc_emu_start(m.uc, pc, 0, 0, 0);
        depuis_uc(m, s);
        total += m.a.n; x87 += m.a.x87;
        if (e != UC_ERR_OK || m.a.intr >= 0 || m.a.invalide || m.a.n > kMaxInstr || m.a.sortie == 0) {
            uint32_t eip = 0;
            uc_reg_read(m.uc, UC_X86_REG_EIP, &eip);
            fprintf(stderr, "[HEAP-CODE] ARRET ANORMAL tid=%d entree=0x%08x eip=0x%08x err=%s intr=%d invalide=%d "
                            "n=%llu der=0x%08x esp=0x%08x\n", win32_current_tid(), entree, eip, uc_strerror(e),
                    m.a.intr, (int)m.a.invalide, (unsigned long long)m.a.n, m.a.der, s.gpr.rsp.dword);
            abort();
        }
        const uint32_t x = m.a.sortie, der = m.a.der;
        const uint8_t op = bo2_g_base[der];
        const bool est_ret = op == 0xC3 || op == 0xC2 || (op == 0xF3 && bo2_g_base[der + 1] == 0xC3);
        if (est_ret) { pc = x; break; }
        const bool est_call = lit32(s.gpr.rsp.dword) == der + m.a.der_taille;
        s._0 = lit32(s.gpr.rsp.dword);          // ce que la cible depilera si elle rend normalement
        appels++;
        if (t_prof >= kAttendusMax) {
            fprintf(stderr, "[HEAP-CODE] tid=%d %d sessions du tas imbriquees : pile des retours attendus pleine\n",
                    win32_current_tid(), t_prof);
            abort();
        }
        t_attendus[t_prof++] = s._0;
        *mem = bo2_heap_code_appel(s, x, *mem);
        --t_prof;
        pc = s._0;
        if (!bo2_est_heap_code(pc)) {
            if (est_call) fprintf(stderr, "[HEAP-CODE] tid=%d l'appel 0x%08x depuis 0x%08x est revenu HORS du tas (0x%08x)\n",
                                  win32_current_tid(), x, der, pc);
            break;
        }
    }
    static int dits = 0;
    if (dits++ < 64)
        fprintf(stderr, "[HEAP-CODE] tid=%d entree=0x%08x sortie=0x%08x instructions=%llu appels=%d x87=%llu "
                        "esp 0x%08x->0x%08x eax=0x%08x\n", win32_current_tid(), entree, pc,
                (unsigned long long)total, appels, (unsigned long long)x87, esp0, s.gpr.rsp.dword, s.gpr.rax.dword);
    return pc;
}
