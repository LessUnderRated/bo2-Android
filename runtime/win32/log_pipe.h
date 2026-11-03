// ── [JOURNAL] le journal ne doit jamais arreter le jeu (Z-NATIF-23b, 23c) ──
// stderr n'a pas de tampon : chaque ligne est un write(2) fait sous le verrou du FILE. Vers un FICHIER, ce write
// bloque par moments 170-430 ms (fil en etat U dans __write_nocancel) et tous les fils qui journalisent attendent
// derriere lui dans flockfile : 6 a-coups en jeu sur 6 au run 045825 (fil de rendu et pompe, piles [A-COUP-FIL]).
// Quand fd 2 est un fichier, il devient donc un tube vers un `cat` enfant qui, lui, ecrit le fichier : un write
// dans un tampon noyau (~1 Mo, ~20 s de journal) ne bloque pas. Un enfant et non un fil : les lignes deja ecrites
// survivent a un crash et a kill -9 (le noyau les garde, cat les vide puis sort a la fin de fichier).
// BO2_JOURNAL_DIRECT=1 = l'ancien chemin (bras temoin). Temoin : « [JOURNAL] par tube ».
// cat a sa propre session : un signal envoye au groupe du jeu (Ctrl-C, fermeture du terminal) ne le tue pas avant
// que le jeu ait ecrit ses rapports de fin ; il ne sort qu'a la fin de fichier.
// ponytail: boot n'attend pas cat a sa sortie (il finit par _exit, pas par atexit) ; qui lit la fin du journal dans
// la milliseconde qui suit la mort de boot peut manquer les dernieres lignes (1 a 4 % des essais hors jeu). Le
// lecteur attend donc un peu (le lanceur de runs) ; si ca mord ailleurs : waitpid dans jrnl_term_handler.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

inline void bo2_journal_par_tube() {
    struct stat s2 {}, s1 {};
    if (fstat(2, &s2) != 0 || !S_ISREG(s2.st_mode)) return;          // terminal ou tube : rien a decoupler
    if (const char* e = getenv("BO2_JOURNAL_DIRECT"); e && atoi(e) > 0) {
        fprintf(stderr, "[JOURNAL] direct (BO2_JOURNAL_DIRECT) : chaque ligne ecrit le fichier\n");
        return;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("[JOURNAL] socketpair"); return; }
    int tampon = 1 << 20, un = 1;
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &tampon, sizeof tampon);
    setsockopt(sv[1], SOL_SOCKET, SO_NOSIGPIPE, &un, sizeof un);     // cat tue : le jeu continue, sans journal
    socklen_t lg = sizeof tampon;
    getsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &tampon, &lg);
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSID);   // trois descripteurs, sa session
    posix_spawn_file_actions_adddup2(&fa, sv[0], 0);
    posix_spawn_file_actions_adddup2(&fa, 2, 1);
    posix_spawn_file_actions_addinherit_np(&fa, 2);
    char* const argv[] = {(char*)"cat", nullptr};
    char* const envp[] = {nullptr};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, "/bin/cat", &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[0]);
    if (rc != 0) { fprintf(stderr, "[JOURNAL] cat non lance (%d) : journal direct\n", rc); close(sv[1]); return; }
    const bool meme_sortie = fstat(1, &s1) == 0 && s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino;
    fflush(stdout);
    dup2(sv[1], 2);
    if (meme_sortie) dup2(sv[1], 1);                                 // `>> journal 2>&1` : l'ordre des lignes tient
    close(sv[1]);
    fprintf(stderr, "[JOURNAL] par tube : cat pid=%d, tampon %d octets%s\n", (int)pid, tampon,
            meme_sortie ? ", stdout aussi" : "");
}
