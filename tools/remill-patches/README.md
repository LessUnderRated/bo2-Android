# Correctifs à appliquer à `tools/remill-src`

`tools/remill-src/` est **gitignoré** (c'est un clone amont). Les correctifs qui
y vivent doivent donc être stockés ici, sinon une reconstruction du lifter les
perd en silence — et le symptôme est un lift qui a l'air de marcher.

## Appliquer

```bash
cd tools/remill-src
git apply ../remill-patches/*.patch
cd build && ninja "$PWD/lib/Arch/X86/Runtime/x86.bc"
```

⚠️ `ninja semantics` **ne suffit pas** : la cible phony ne dépend pas de
`x86.bc`, et `ninja` répond « no work to do » même après un `rm` du fichier.
Nommer la cible par son chemin absolu.

Le lifter binaire (`build/bin/lift/remill-lift-17`) n'a **pas** besoin d'être
relié : il lit `build/lib/Arch/X86/Runtime/x86.bc` au démarrage
(`REMILL_BUILD_SEMANTICS_DIR_X86` est compilé vers le répertoire de build).

## 0001 — `DEF_ISEL(POPA_16)` manquant

`InstructionFunctionName` (lib/Arch/X86/Arch.cpp:338) suffixe un iform marqué
SCALABLE par sa **largeur d'opérande effective**. Donc :

| octets | iform XED | nom d'ISEL | état amont |
|---|---|---|---|
| `66 60` (pushaw) | PUSHA | `PUSHA_16` | ✅ défini |
| `60` (pushad) | PUSHAD | `PUSHAD_32` | ✅ défini |
| `66 61` (popaw) | POPA | `POPA_16` | ❌ **absent** |
| `61` (popad) | POPAD | `POPAD_32` | ✅ défini |

`DEF_ISEL(POPA_32)` existait — un nom qu'aucune instruction décodable ne
produit. Sans `POPA_16`, remill abaisse `popaw` en
`__remill_sync_hyper_call(..., kX86EmulateInstruction=0x100)` suivi de
`__remill_error`, **et la fonction s'arrête là**.

Mesuré : `sub_64fef0` (la détection CPU, `pushaw/cpuid/.../popaw`) passe de
23 blocs / 547 lignes d'IR à **153 blocs / 3 512 lignes**, et de 2
`__remill_error` à **0**. C'est la cause de « le relift complet casse le
démarrage, 3/3 » : mort à 5 s sur `[__remill_error] pc=0x0064ff33`.

## 0002 — `CVTPI2PS` : destination `rw`, trois opérandes décodés

XED marque la destination de `cvtpi2ps xmm, mm/m64` en lecture-écriture (les 64
bits hauts sont conservés). `DecodeRegister` (lib/Arch/X86/Arch.cpp) pousse donc
**deux** opérandes pour elle (écriture puis lecture), puis la source : trois en
tout. La sémantique amont `CVTPI2PS(V128W dst, S src)` n'en prend que deux, donc
aucune instruction décodée ne lui correspond et remill abaisse chaque
`cvtpi2ps` en `__remill_error` (même famille que `CVTSI2SS`, qui a bien
`V128W dst, V128 src1, S2 src2`).

Mesuré : `0f 2a c1`, `0f 2a c8` et `0f 2a 04 24` passent de 1 `__remill_error` à
0 et à deux `sitofp`, avec les 64 bits hauts de XMM recopiés. BO2 a **24**
`cvtpi2ps` dans tout `.text`, tous dans les six routines de dépouillement CPU
(0x707540, 7077d0, 707a40, 707d50, 708050, 708400). Chaque vertex passait par
`__remill_error` puis par un bloc de reprise (`emulate_cvtpi2ps_and_resume`,
win32_runtime.cpp) : 0,09 cœur dans `__remill_error` + ~0,07 dans
`__remill_missing_block` (sample P676).

Recette C36 (les six routines, aucun lift complet) :

```bash
BO2_INLINE_READS=1 BO2_INLINE_FLAGS=1 python3 tools/lift_extra.py 707540 7077d0 707a40 707d50 708050 708400
# puis, pour chaque VA, la chaine de lifted.o (comme tools/jq_impl_folded.txt, sans bo2natomic : 0 lock) :
llvm-dis -o - boot_cache/lift_wip/func_<va>.bc.fixed | python3 scripts/fold/stream_inline_triv.py \
  | python3 scripts/fold/stream_c27.py | python3 scripts/fold/stream_inline_writes.py \
  | python3 scripts/fold/dedup_ll_globals.py | python3 scripts/fold/widen_guest_gep.py | llvm-as -o nw.bc
clang -arch arm64 -c -O2 -x ir -o nw.o nw.bc
llvm-objcopy --redefine-sym _sub_<va>=_sub_<va>_impl nw.o boot_cache/c36_<va>_impl.o
```

tools/boot.py (édition de liens) lie les six objets et redirige `_sub_<va>` → `_sub_<va>_impl`
(échec du build si une redirection manque). Les blocs de reprise de
tools/extra_lifted_vas.txt restent enregistrés mais ne sont plus atteints.
Témoin : 0 ligne `[CVTPI2PS` au journal (4 par run avant), et `[SKIN]`
(BO2_SKIN_COUNT) se tait avec lui.

## 0003 — `FLDCW` : bit 6 réservé du mot de contrôle x87

Sur le matériel, le bit 6 du mot de contrôle x87 est réservé et se relit
toujours à 1 (Bochs : FLDCW force 0x0040). Le `_control87` du CRT de BO2
(0x985a1b) assemble son mot sans ce bit, puis fait `fldcw` en 0x985aec. Chez nous
le fil principal gardait donc **0x003F**. Or le CRT teste `fnstcw & 0x7f == 0x7f`
(22 sites, 21 fonctions mathématiques, dont `ceil` 0x9620d0) pour choisir son
chemin SSE2 : le fil principal prenait le chemin x87, dont l'arrondi n'est pas
émulé (`__remill_fpu_set_rounding` vide, C26). Résultat : gel de haiti en srvbe
(P759).

Le patch fait `cw.flat = Read(cwd) | 0x40`. Seul `_control87` est relifté
(recette C36 de 0002, puis une passe d'inlining des retours avant `dedup_ll_globals.py`) en
`boot_cache/x87_985a1b_impl.o`, redirigé par tools/boot.py (édition de liens). Témoin :
`[CW] (niveau) tid=0 cw=0x007f` (P761).

⚠️ Une fois ce chemin pris par le fil principal, trois corps SSE2 du CRT que
l'ancien lift (`boot_cache/lift_tx`) avait tronqués sur un `movapd` décodé
invalide (`store i32 32, ptr %state`, puis `__remill_error` et signal 6) sont
devenus atteignables : 0x9817a8, 0x981958 (`exp`), 0x9839c8. Ils ont été
reliftés de la même façon (P762).

## 0004 — `FIST*`, `FRNDINT`, `FBSTP` : l'arrondi suit le champ RC du mot de contrôle x87

Amont : ces instructions arrondissent par `FRoundUsingMode` = `__builtin_nearbyint`, au mode d'arrondi de
l'HÔTE, que `FLDCW` est censé régler par `__remill_fpu_set_rounding`. Chez nous cet intrinsèque est vide (runtime) et
replié en `ret void` (`tools/boot.py`, C26) : tout `fistp` arrondissait au plus proche, quel que soit le mot chargé.
Or le cast tronquant de MSVC est `fnstcw ; or eax, 0xc00 ; fldcw ; fistp ; fldcw` (672 `fldcw` dans t6sp, 253 dans
t6zm) : 2,7 donnait 3.

Le patch lit `state.x87.fxsave.cwd.rc` (par fil invité, sans toucher au FPCR hôte) : troncature, plancher, plafond,
ou `nearbyint` pour RC = 0. `FISTTP` tronquait déjà (`TruncateToInt`).

Premier défaut corrigé avec lui (Z-NATIF-16k) : le codec réseau des flottants bornés de t6zm (écrivain 0x58a040,
lecteur 0x567440) quantifie par `(v − min)·pas + 0,5` tronqué ; arrondi au plus proche, serveur et client ne
retombent plus sur le même entier et le client reçoit une hauteur de vue fausse (10,95 au lieu de 11,00) → la
prédiction rejoue la descente « couché » à chaque instantané (tremblement). Seules ces deux fonctions sont
reliftées. Les autres sites `fldcw` gardent l'ancien lift tant
qu'ils ne sont pas reliftés : un relift large se décide à part.

`x86.bc` d'avant : `boot_cache/x86.bc.avant_0004`.

Campagne (P810, 2026-10-10) : même codec, écrivain 0x5648c0 et lecteur 0x42ea10, reliftés par la recette C36
(+ la passe d'inlining des retours) en `boot_cache/x87_5648c0_impl.o` et `x87_42ea10_impl.o`, redirigés par
tools/boot.py (édition de liens).

## 0005 — `remill-lift --serve` : la sémantique chargée une fois, un fork par fonction

Le lift d'une fonction ordinaire dure ~110 ms, dont ~90 à charger `x86.bc` (`LoadArchSemantics`) : un `c3` seul
prend déjà 90 ms. `--serve` charge la sémantique, puis lit une demande par ligne sur stdin
(`octets⇥adresse hex⇥bc_out⇥ir_out⇥fichier stderr`) et `fork()` à cet endroit précis : l'enfant est exactement
un processus neuf arrivé là, avec les drapeaux de sa demande, et fait le reste du lift comme avant. Il imprime
`pid <n>` (pour le plafond de temps et de mémoire de `tools/boot.py`) puis `exit <statut>`.

Mesure P1107 (186 fonctions sps) : remill 113 → 15 ms par fonction ; `.bc` et `.ll` identiques à l'octet à ceux
d'un remill neuf. `tools/boot.py` s'en sert quand `--help` cite `-serve`, sinon il lance un remill par fonction.
Le binaire seul change (`ninja -C build bin/lift/remill-lift-17`, quelques secondes), pas `x86.bc`.
