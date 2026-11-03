// diag_counter.h — compteur de diagnostic par clé, sûr entre fils hôtes.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// 🎯 P660/C14 — UNE TABLE DE DIAGNOSTIC NE SUPPORTE PAS DEUX FILS HÔTES.
//
// `static std::unordered_map<…> m; if (m[k]++ == 0) fprintf(…)` était sûr tant
// qu'un seul fil hôte exécutait l'invité. Hébergées, plusieurs coroutines
// appellent Sleep / SetEvent / WaitForSingleObject EN MÊME TEMPS : deux
// insertions concurrentes corrompent la table, et un rehash sur une chaîne
// corrompue ne termine plus.
//
// Run p659 204958 (srvbe) : le fil jq t7 passe 6678/6678 échantillons `sample`
// dans __hash_table::__do_rehash appelé par impl_Sleep ; sa tâche ne finit
// jamais, Main l'attend, et le jeu gèle à la fin de la vidéo d'intro. Témoin de
// la course : la ligne « première fois » `[Sleep] caller-ret=0x0056123c`
// imprimée DEUX fois (2 runs srvbe sur 3, 0 sur 5 runs srv/C13).
//
// Remplacement : adressage ouvert de taille fixe, CAS sur la clé, fetch_add sur
// le compte. Ni rehash ni verrou. Table pleine : bump rend kFull et l'appelant
// n'imprime pas (ces journaux sont déjà plafonnés).
template <unsigned kLog2>
class DiagCounter {
public:
    static constexpr uint32_t kFull = UINT32_MAX;

    // Rend le compte AVANT incrément (0 la première fois) ou kFull.
    // La clé UINT64_MAX est réservée (stockée +1 ; 0 = case libre).
    uint32_t bump(uint64_t k) {
        const uint64_t tag = k + 1;
        size_t i = static_cast<size_t>((tag * 0x9E3779B97F4A7C15ull) >> (64 - kLog2));
        for (size_t essai = 0; essai < kSize; ++essai, i = (i + 1) & (kSize - 1)) {
            uint64_t cur = key_[i].load(std::memory_order_acquire);
            if (cur == 0) {
                uint64_t attendu = 0;
                cur = key_[i].compare_exchange_strong(attendu, tag, std::memory_order_acq_rel)
                          ? tag : attendu;
            }
            if (cur == tag) return n_[i].fetch_add(1, std::memory_order_relaxed);
        }
        return kFull;
    }

    // Parcours sans verrou (instantané approximatif) : f(clé, compte) pour chaque case prise.
    template <class F> void for_each(F&& f) const {
        for (size_t i = 0; i < kSize; ++i) {
            const uint64_t tag = key_[i].load(std::memory_order_acquire);
            if (tag) f(tag - 1, n_[i].load(std::memory_order_relaxed));
        }
    }

private:
    static constexpr size_t kSize = size_t{1} << kLog2;
    std::atomic<uint64_t> key_[kSize] = {};
    std::atomic<uint32_t> n_[kSize] = {};
};
