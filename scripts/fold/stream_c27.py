#!/usr/bin/env python3
"""C27 — deux reecritures EXACTES de l'IR lifte, en flux (stdin -> stdout), apres C26.

1. Retire `call void asm sideeffect "", "~{memory},..."()` : c'est BarrierReorder() de remill
   (Operators.h, utilise seulement par CheckedFloat* de FLAGS.cpp), qui ordonne l'operation
   flottante contre __remill_fpu_exception_test/clear. C26 a replie ces deux-la en constantes
   (0, vide) : la barriere n'ordonne plus rien, mais elle reste une barriere memoire complete
   pour l'optimiseur (l'etat doit etre ecrit avant, relu apres). Remill prevoit ce retrait
   (REMILL_BARRIER_AS_NOP).
2. Decoupe chaque `llvm.memcpy(dst, src, 10, non volatile)` en copie i64 + i16. Ce sont les
   rotations de la pile x87 (PUSH/POP_X87_STACK, float80_t = 10 octets sur l'hote ARM) :
   InstCombine ne replie pas une taille non puissance de 2 et GVN ne transmet pas un memcpy
   a un chargement `double` de 8 octets, donc les 8 fentes restent copiees en memoire a
   chaque empilement. Memes octets ; memcpy interdit le recouvrement, l'ordre est libre.

Local a la ligne. Resume sur stderr : `[c27] <asm retirees> asm, <memcpy decoupes> memcpy`.
"""
import re
import sys

ASM = 'call void asm sideeffect "", "~{memory},~{dirflag},~{fpsr},~{flags}"()'
CPY = re.compile(r'^(\s*)(?:tail |notail )?call void @llvm\.memcpy\.p0\.p0\.i(?:32|64)\('
                 r'ptr [^%,]*(%[-\w.$]+), ptr [^%,]*(%[-\w.$]+), i(?:32|64) 10, i1 false\)'
                 r'(?:\s*#\d+)?\s*$')
n_asm = n_cpy = 0
out = sys.stdout
for ligne in sys.stdin:
    if ASM in ligne:
        corps = ligne.strip()
        if re.fullmatch(r'(?:tail |notail )?' + re.escape(ASM) + r'(?:\s*#\d+)?', corps):
            n_asm += 1
            continue
    elif "@llvm.memcpy" in ligne and "10, i1 false)" in ligne:
        m = CPY.match(ligne)
        if m:
            ind, d, s = m.groups()
            n_cpy += 1
            k = f"%__c27_{n_cpy}"
            out.write(f"{ind}{k}.q = load i64, ptr {s}, align 2\n"
                      f"{ind}{k}.s8 = getelementptr inbounds i8, ptr {s}, i64 8\n"
                      f"{ind}{k}.h = load i16, ptr {k}.s8, align 2\n"
                      f"{ind}store i64 {k}.q, ptr {d}, align 2\n"
                      f"{ind}{k}.d8 = getelementptr inbounds i8, ptr {d}, i64 8\n"
                      f"{ind}store i16 {k}.h, ptr {k}.d8, align 2\n")
            continue
    out.write(ligne)
sys.stderr.write(f"[c27] {n_asm} asm, {n_cpy} memcpy\n")
