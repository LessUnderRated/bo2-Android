// bo2_hot.h — UNE LIGNE DE CACHE A SOI pour chaque global du chemin chaud.
//
// 🎯 P656/C10 — LE FAUX PARTAGE DES FILS HEBERGES. Le code leve (tools/boot.py,
// BO2_INLINE_READS/WRITES) charge a CHAQUE acces invite `bo2_g_base`,
// `bo2_w32_cold`, `bo2_w32_slow_map`, `g_park_pending`, et a chaque ecriture 32
// bits la queue de preemption (`g_w32_preempt_every`, `g_any_hosted`,
// `bo2_sched_tp`, `g_w32_preempt_left`). L'editeur de liens les avait semes au
// hasard des lignes de 128 octets (nm, binaire d00d1ce8) :
//   * `bo2_g_base` partageait sa ligne avec `g_guest_writes`, que TOUS les fils
//     incrementent a chaque ecriture invitee (~105 M/s) : un fil heberge qui lit
//     la memoire invitee rate son L1 des qu'un autre fil vient d'ecrire ;
//   * `bo2_w32_slow_map`/`bo2_w32_cold`/`g_w32_armed` partageaient la leur avec
//     `g_mtl_mu`, le mutex Metal pris par la pompe ;
//   * `g_w32_preempt_left` (ecrit par l'ordonnanceur a chaque ecriture) avec
//     `g_cmdq_mu` et des compteurs du son ;
//   * `g_park_pending` avec `g_ev_set[]`, incremente par chaque SetEvent.
// Sur un seul fil ce n'est rien ; heberge, chaque acces peut devenir un transfert
// de ligne entre coeurs. Aucune semantique ne change : memes noms, memes types,
// seules les adresses bougent.
//
// Section propre + alignement 128 : chaque variable commence une ligne et la
// suivante de la section commence a la ligne d'apres. Le bourrage BO2_HOT_PAD,
// defini APRES les globaux chauds de chaque unite, ferme la section pour qu'aucune
// donnee d'une autre section ne vienne partager la ligne du dernier. ⚠️ L'ordre
// des atomes est celui de l'emission : le verifier dans `nm -n boot_cache/boot`
// apres chaque build (P656a), pas le supposer.
//
// ⚠️ `g_guest_writes` reste PARTAGE pour de vrai (tous les fils l'incrementent) :
// l'isoler protege les autres, pas lui. Le retirer du chemin des fils heberges
// demande de refaire lifted.o (le compteur est replie dans l'IR).
#pragma once

// Mach-O names a section "segment,section"; on ELF it is one name (clang quotes the Mach-O form for ELF, GCC does
// not, and GNU as rejects it).
#if defined(__APPLE__)
#define BO2_HOT_SECTION "__DATA,__bo2_hot"
#else
#define BO2_HOT_SECTION ".data.bo2_hot"
#endif
#define BO2_HOT_LINE __attribute__((section(BO2_HOT_SECTION), aligned(128)))
#define BO2_HOT_PAD(tag)                                                        \
    __attribute__((used, section(BO2_HOT_SECTION), aligned(128)))               \
    static char bo2_hot_pad_##tag[128]
