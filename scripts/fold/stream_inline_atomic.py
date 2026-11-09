#!/usr/bin/env python3
"""Android — les sequences `lock` du code leve prennent la rayure de LEUR adresse, plus le verrou global. EN FLUX.

Remill lifte une instruction `lock` (lock add/xadd/inc/dec/or/and/xchg, lock cmpxchg...) ainsi :
    %b = call ptr @__remill_atomic_begin(ptr %m)        <- aucune adresse : le runtime ne pouvait prendre
    ... calcul de l'adresse ...                             qu'UN verrou pour toutes les instructions `lock`
    %v = call iN @__remill_read_memory_N(ptr, i32 %a)   (ou compare_exchange_memory_N, ou write_memory_N)
    ...
    call ptr @__remill_atomic_end(ptr ...)              (pour cmpxchg, dans un AUTRE bloc : la branche succes/echec)

Ce filtre, dans le bloc de base du `begin` :
  - remplace le `begin` par un alias inerte de son operande (`getelementptr i8, ptr %m, i64 0`) ;
  - pose `call void @bo2_atomic_lock_at(i32 %a)` juste avant le PREMIER acces memoire invite qui suit, dont %a est
    l'adresse (il la domine, puisque c'est un operande de cet acces).
Le `end` ne change pas : le runtime (win32_runtime.cpp, rayures, Android) sait par fil quelle rayure il tient.
Un `begin` sans acces memoire dans son bloc reste tel quel : il prend toutes les rayures, l'ancien verrou global.

Rien n'est deplace : entre `begin` et le premier acces il n'y a que des calculs d'adresse et des lectures de
registres (l'etat `State`), jamais d'acces a la memoire invitee. bo2_atomic_lock_at est un appel opaque avec une
barriere complete : ni clang ni le processeur ne remontent l'acces au-dessus.
"""
import re
import sys

BEGIN = re.compile(r'^(\s*)(%[\w.$-]+) = (?:tail |notail |musttail )?call ptr @__remill_atomic_begin\(ptr(?: noundef)? ([^,)]+)\)')
MEMOP = re.compile(r'@__remill_(?:read_memory|write_memory|compare_exchange_memory|fetch_and_[a-z]+)_'
                   r'(?:8|16|32|64|128|f32|f64|f80)\(ptr(?: noundef)?(?: nonnull)? [^,]+, i32(?: noundef)? ([^,)\s]+)')
LABEL = re.compile(r'^[\w.$"-]+:')
TERMS = ("br ", "ret ", "ret\n", "switch ", "unreachable", "indirectbr ", "resume ", "callbr ")

out = sys.stdout
held = None          # [indent, name, operand, buffered lines]
n_rw = n_left = 0


def flush_unchanged():
    global held, n_left
    if held is not None:
        out.write(held[4])
        out.writelines(held[3])
        n_left += 1
        held = None


for line in sys.stdin:
    if held is not None:
        s = line.lstrip()
        ends_block = (LABEL.match(line) is not None or line.startswith("}") or s.startswith(TERMS)
                      or BEGIN.match(line) is not None)
        m = None if ends_block else MEMOP.search(line)
        if m:
            ind, name, operand = held[0], held[1], held[2]
            out.write("%s%s = getelementptr i8, ptr %s, i64 0\n" % (ind, name, operand))
            out.writelines(held[3])
            out.write("%scall void @bo2_atomic_lock_at(i32 %s)\n" % (ind, m.group(1)))
            out.write(line)
            n_rw += 1
            held = None
            continue
        if not ends_block:
            held[3].append(line)
            continue
        flush_unchanged()
        # fall through: this line may itself be a new begin
    b = BEGIN.match(line)
    if b:
        held = [b.group(1), b.group(2), b.group(3).strip(), [], line]
        continue
    out.write(line)

flush_unchanged()
if n_rw:
    out.write("\ndeclare void @bo2_atomic_lock_at(i32)\n")
sys.stderr.write("[atomic] %d sequence(s) lock sur leur rayure, %d laissee(s) au verrou global\n" % (n_rw, n_left))
