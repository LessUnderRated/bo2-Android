#!/usr/bin/env python3
"""Android — dire a LLVM que la memoire invitee et les chemins lents ne touchent pas la `State`. EN FLUX, en dernier.

Pourquoi. Le code leve garde les registres x86 et les drapeaux dans la `State` (argument `noalias %state`). Les acces
a la memoire invitee passent par un `inttoptr` (base + adresse) : pour LLVM, un tel pointeur peut designer
n'importe quoi de capture, `State` comprise (elle est passee aux autres fonctions levees). Alors, autour de CHAQUE
acces invite, LLVM relit les registres depuis la `State`, y range le compteur de programme de chaque instruction, et
apres chaque chemin lent (un appel froid qui rejoint le chemin rapide) il recharge tout. Mesure sur sub_4589a0 (S26) :
183 rangements du PC, 97 relectures d'esp, ~60 rangements par drapeau, pour une fonction de 6 855 instructions.
Le portage Skyrim (ttom795/Skyrim_Android) garde registres et drapeaux dans des locales C : c'est l'ecart principal.

Quoi. Des metadonnees d'alias a portee (scoped noalias), sans rien deplacer soi-meme :
  - chaque load/store dont le pointeur derive de %state (getelementptr en chaine) : `!alias.scope` « bo2.state » ;
  - chaque load/store VOLATILE (dans le code leve, ce sont exactement les acces a la memoire invitee : les replis
    de lecture et d'ecriture les emettent volatils), et chaque appel aux chemins lents de la memoire, au tic de
    preemption et aux verrous `lock` : `!noalias` « bo2.state ». (Pas les compare-exchange : cas_spin_note y lit le
    compteur d'ecritures de la coroutine dans la `State`, voir plus bas.)
Les appels aux autres fonctions levees et aux entrees du runtime qui lisent la `State` (function_call, jump,
function_return_slow, error, hyper_call...) ne sont PAS marques : la `State` est rangee avant eux, comme avant.

Ce qui le rend sur (verifie dans le runtime, 2026-11-08) : les chemins lents de la memoire ne recoivent pas la
`State` et ne l'ecrivent pas ; rien n'ecrit la `State` d'une coroutine suspendue (pas de Get/SetThreadContext,
SuspendThread est un bouchon) ; le gestionnaire de SIGSEGV ne fait que diagnostiquer (la carte invitee de 4 Gio est
entierement reservee, aucun acces invite n'y faute). Une sonde de diagnostic qui lit la `State` d'une coroutine
pendant un acces peut y voir un registre en retard : c'est le seul prix.
"""
import re
import sys

DOM, SCOPE, LIST = "!880001", "!880002", "!880003"
DEFINE = re.compile(r'^define\b')
STATE_DEF = re.compile(r'\bptr noalias %state\b')
GEP = re.compile(r'^\s*(%[\w.$-]+) = getelementptr\b[^,]*, ptr (%[\w.$-]+)')
LOAD = re.compile(r'^\s*%[\w.$-]+ = load (volatile )?[^,]+, ptr (%[\w.$-]+|@[\w.$-]+)')
STORE = re.compile(r'^\s*store (volatile )?.*?, ptr (%[\w.$-]+|@[\w.$-]+)(,|$)')
CALLS = re.compile(r'call [^@]*@(__remill_(?:read|write)_memory_\w+_slow|__remill_(?:read|write)_memory_(?:8|16|32|64|f32|f64|f80)'
                   r'|win32_thread_write_tick|bo2_atomic_lock_at'
                   r'|__remill_atomic_(?:begin|end)|__remill_fetch_and_\w+)\(')

# The coroutine's guest-write count, in X86State::_padding (offset 2760, 8 bytes Remill's semantics never touch),
# bumped after each guest write call site of a lifted function. It replaces the shared g_guest_writes counter the
# write fold used to bump (boot.py _count_guest_writes, Android): cas_spin_note's "did this coroutine write anything
# between two failed cmpxchg" (runtime, win32_runtime.cpp) reads it from the running coroutine's State. As a State
# access it is in the bo2.state scope: LLVM keeps it in a register between calls and folds the increments together,
# and stores it before any call that may read State. compare_exchange is such a call (it is not exempt above).
WRITECALL = re.compile(r'call [^@]*@__remill_write_memory_(?:8|16|32|64|f32|f64|f80)\(')
WCOUNT_OFF = 2760
n_wcount = 0

out = sys.stdout
in_fn = False
lifted = False
state_ptrs = set()
n_scope = n_noalias = n_calls = 0


def tag(line, md):
    if "!alias.scope" in line or "!noalias" in line:
        return line, False
    return line.rstrip("\n") + ", " + md + "\n", True


# Et deux choses vraies que LLVM ne peut pas deviner (meme mesure : -12 % d'instructions, -30 % d'acces memoire
# avec le marquage ci-dessus) :
#  - les globaux du runtime que lit le code leve (bo2_*, g_*) sont definis dans libmain.so meme et y sont caches
#    (--exclude-libs,ALL) : `dso_local`, donc adrp + ldr direct au lieu de passer par la GOT (3 instructions par
#    acces). Un symbole qui ne le serait pas ferait echouer l'edition de liens, pas l'execution ;
#  - bo2_g_base (posee a l'init, avant tout code leve) et bo2_w32_slow_map (adresse d'un tableau statique) ne
#    changent jamais : `!invariant.load`, chargees une fois par fonction au lieu d'une fois par acces.
RTGLOBAL = re.compile(r'^(@(?:bo2_|g_)[\w.$]*) = external global\b')
INVARIANT = re.compile(r'^(\s*%[\w.$-]+ = load ptr, ptr @(?:bo2_g_base|bo2_w32_slow_map), align 8)\b')
INV_MD = "!880009"
n_dso = n_inv = 0

for line in sys.stdin:
    r = RTGLOBAL.match(line)
    if r:
        line = RTGLOBAL.sub(r'\1 = external dso_local global', line, count=1)
        n_dso += 1
    if in_fn:
        v = INVARIANT.match(line)
        if v and "!invariant.load" not in line:
            line = line.rstrip("\n") + ", !invariant.load " + INV_MD + "\n"
            n_inv += 1
    if DEFINE.match(line):
        in_fn = True
        lifted = STATE_DEF.search(line) is not None
        state_ptrs = {"%state"} if lifted else set()
        out.write(line)
        continue
    if in_fn and line.startswith("}"):
        in_fn = False
        out.write(line)
        continue
    if not in_fn:
        out.write(line)
        continue
    g = GEP.match(line)
    if g and g.group(2) in state_ptrs:
        state_ptrs.add(g.group(1))
        out.write(line)
        continue
    m = LOAD.match(line) or STORE.match(line)
    if m:
        volatile, ptr = m.group(1), m.group(2)
        if volatile:
            line, ok = tag(line, "!noalias " + LIST)
            n_noalias += ok
        elif ptr in state_ptrs:
            line, ok = tag(line, "!alias.scope " + LIST)
            n_scope += ok
        out.write(line)
        continue
    if CALLS.search(line):
        line, ok = tag(line, "!noalias " + LIST)
        n_calls += ok
    out.write(line)
    if lifted and WRITECALL.search(line):
        k = n_wcount
        n_wcount += 1
        out.write("  %%bo2wc.%d = getelementptr inbounds i8, ptr %%state, i64 %d\n" % (k, WCOUNT_OFF))
        out.write("  %%bo2wv.%d = load i64, ptr %%bo2wc.%d, align 8, !alias.scope %s\n" % (k, k, LIST))
        out.write("  %%bo2wn.%d = add i64 %%bo2wv.%d, 1\n" % (k, k))
        out.write("  store i64 %%bo2wn.%d, ptr %%bo2wc.%d, align 8, !alias.scope %s\n" % (k, k, LIST))

out.write('\n%s = distinct !{%s, !"bo2.state.domain"}\n' % (DOM, DOM))
out.write('%s = distinct !{%s, %s, !"bo2.state"}\n' % (SCOPE, SCOPE, DOM))
out.write('%s = !{%s}\n' % (LIST, SCOPE))
out.write('%s = !{}\n' % INV_MD)
sys.stderr.write("[scope] %d acces State, %d acces invites, %d appels marques ; %d globaux dso_local, %d bases invariantes ; "
                 "%d ecritures comptees dans la State\n" % (n_scope, n_noalias, n_calls, n_dso, n_inv, n_wcount))
