#!/usr/bin/env python3
"""
boot — the truth test. Lift the entry chain, map the real PE sections,
set up a fake TEB/PEB, and execute the entry point. Trace every import
and stub call to see how far the game gets before crashing.
"""
import os, sys, subprocess, tempfile, csv, re, time, struct, glob, select
from concurrent.futures import ProcessPoolExecutor, wait, FIRST_COMPLETED
import multiprocessing
import faulthandler, signal
# `kill -USR1 <pid>` prints where this process waits, lift workers included (fork keeps it): build_steam.sh's watchdog
# sends it before killing a lift that stopped moving (issue #4).
faulthandler.register(signal.SIGUSR1, all_threads=True)
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM

# Self-recursion de-hang. remill-lift's trace lifter FOLLOWS direct calls whose
# target lies inside the byte range it's lifting; for a self-recursive function
# (a `call <own-start>`) that expands forever and hangs with no output. ALL 115
# pre-seeded "monster" .fail markers are exactly this shape. Fix: before lifting,
# retarget each intra-range direct call to <target>+SELF_CALL_TAG so it lands
# OUTSIDE the range and remill emits an external @sub_<target+TAG> call instead;
# fix_ir_text() renames that symbol back to @sub_<target> (a real, in-module or
# linked function), restoring correct recursion. The .text max is 0xa314b0, so
# any @sub_ >= SELF_CALL_TAG in the IR is unambiguously one of these tags.
SELF_CALL_TAG = 0x10000000
_md_patch = Cs(CS_ARCH_X86, CS_MODE_32)
_md_patch.detail = True

def patch_intra_calls(base_bytes, va, size):
    """Return base_bytes with every intra-range `call rel32` retargeted +TAG.
    Only near-call (0xE8) rel32 forms are patched; anything else is left as-is."""
    buf = bytearray(base_bytes)
    n = 0
    for x in _md_patch.disasm(bytes(buf[:size]), va):
        if x.mnemonic != "call" or not x.operands or x.operands[0].type != CS_OP_IMM:
            continue
        t = x.operands[0].imm
        if not (va <= t < va + size):
            continue
        off = x.address - va
        if off + 5 > size or buf[off] != 0xE8:
            continue
        newrel = ((t + SELF_CALL_TAG) - (x.address + 5)) & 0xffffffff
        buf[off + 1:off + 5] = newrel.to_bytes(4, "little")
        n += 1
    return bytes(buf), n

# ...mais le renommage de fix_ir_text ne touche que le SYMBOLE. remill calcule aussi le
# pc passe a l'appele comme `pc courant + (cible+TAG - pc)`, et l'appele s'en sert de
# base pour tout ce qu'il empile : chaque adresse de retour de son sous-arbre d'appels
# directs portait +0x10000000 (P716 : _alloc_osfhnd -> sub_a90280 -> _unlock rend
# a 0x10a90287). Tant que les `ret` ignoraient leur cible, ca ne se voyait pas ; un jeu
# qui lit ses retours (CEG) voyait des adresses hors de l'image.
# Apres opt, l'argument pc d'un `@sub_` est `%R = add i32 %base, N` : un decalage legitime
# tient dans l'image (< 16 Mo), un decalage marque tombe a TAG +/- 16 Mo.
_SUB_CALL_PC = re.compile(r'@sub_[0-9a-f]+\(ptr [^,]*, i32 (%[-\w.]+), ')

def untag_self_call_pcs(ir_text):
    """Retire SELF_CALL_TAG du pc passe aux appeles marques. Rend (texte, nb corriges)."""
    lo, hi = SELF_CALL_TAG - 0x1000000, SELF_CALL_TAG + 0x1000000
    parts = ir_text.split("\ndefine ")
    n = 0
    for i, body in enumerate(parts):
        for arg in set(_SUB_CALL_PC.findall(body)):
            pat = re.compile(r'^(\s*' + re.escape(arg) + r' = add (?:nuw |nsw )*i32 [^,]+, )(-?\d+)$', re.M)
            m = pat.search(body)
            if m and lo <= int(m.group(2)) < hi:
                body = body[:m.start(2)] + str(int(m.group(2)) - SELF_CALL_TAG) + body[m.end(2):]
                n += 1
        parts[i] = body
    return "\ndefine ".join(parts), n

def untag_fixed(fixed_bc):
    """Applique untag_self_call_pcs a un .fixed (bitcode apres opt), en place et atomiquement."""
    r = subprocess.run([LLVM17 + "/bin/llvm-dis", "-o", "-", fixed_bc], capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        return -1
    txt, n = untag_self_call_pcs(r.stdout)
    if n:
        tmp = fixed_bc + ".untag"
        a = subprocess.run([LLVM17 + "/bin/llvm-as", "-o", tmp, "-"], input=txt, capture_output=True, text=True, timeout=30)
        if a.returncode != 0:
            return -1
        os.replace(tmp, fixed_bc)
    return n

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

HERE = os.path.dirname(os.path.abspath(__file__))
REMILL = os.path.join(HERE, "remill-src/build/bin/lift/remill-lift-17")
LLVM17 = "/opt/homebrew/opt/llvm@17"
REM = os.path.join(HERE, "remill-src")
# ── WHICH EXECUTABLE ────────────────────────────────────────────────────────
# BO2_TARGET=sps|mps|zms (default sps): the three Steam executables, after the
# player removed SteamStub from their own copy. ENTRY_VA is the real CRT entry
# (`call X; jmp Y`) of each unpacked image.
_GAME = next((open(p).read().strip() for p in map(os.path.expanduser,
              ("~/.bo2-native/game_dir", "~/.bo2-native/jeu")) if os.path.exists(p)),
             "/Users/Shared/Call of Duty - Black Ops 2")
TARGET = os.environ.get("BO2_TARGET", "sps").lower()
_TARGETS = {
    # Campagne STEAM (build 2026), image décapsulée hors dépôt. Adresses : tools/steam/sp_redirects.tsv (vérifiées au
    # code). Catalogue : tools/ghidra-sps (tools/ghidra_csv.py). Pas de sondes EH : l'exe Steam n'a ni .conceal ni
    # l'enveloppe `call; cmp eax,1; jne; push eax; call` de 0x8ce830 (0 motif). Premier lien : longjmp, inflate et glob
    # (shims sans adresse, motif unique), CRT ; les autres natifs restent à brancher.
    "sps": dict(exe="t6sp.exe", path=os.path.expanduser("~/.bo2-native/steam/t6sp.exe.unpacked.exe"),
                out="bin/ghidra-out-sps", entry=0x923a4c, eh={}, longjmp=0x91ee30,
                redirects={0x77b960: "_sub_4595f0", 0x74f600: "_sub_613a20",
                           0x91a160: "_bo2_memcpy", 0x917b10: "_bo2_memmove", 0x917960: "_bo2_memset",
                           0x91a71e: "_bo2_libm_atan2", 0x88bbf0: "_bo2_vpx_decode",
                           # prédicat du Sound Mix (t6sp 0x4725d0) : sans OnBufferEnd l'intro ne finit jamais (P962)
                           0x7fdd10: "_bo2_snd_ready",
                           # Natifs de débit (P1053), replis _sub_<va>_impl : tools/steam/native_impl.sh.
                           0x69a9a0: "_bo2_snd_s16tof", 0x7fe6a5: "_bo2_snd_addps", 0x7fea7d: "_bo2_snd_biquad",
                           0x47b080: "_bo2_skin1", 0x47b4e0: "_bo2_skin2", 0x47ba60: "_bo2_skin3", 0x47c110: "_bo2_skin4"}),
    # Multi et Zombies STEAM (build 2026) : tools/steam/{mp,zm}_redirects.tsv (vérifiées au code), catalogue
    # tools/ghidra-{mps,zms}. Premier lien : les natifs de correction seulement (memcpy dont
    # le saut de table sort du lift P817, memset, atan2 `rcpss`, VP8, prédicat du Sound Mix) ; ceux de débit (culling,
    # dépouillement) après.
    "mps": dict(exe="t6mp.exe", path=os.path.expanduser("~/.bo2-native/steam/t6mp.exe.unpacked.exe"),
                out="bin/ghidra-out-mps", entry=0xa2ebcc, eh={}, longjmp=0xa28540,
                redirects={0xa23ac0: "_bo2_memcpy", 0xa22ca0: "_bo2_memcpy", 0xa22af0: "_bo2_memset",
                           0xa245b8: "_bo2_libm_atan2", 0x9697c0: "_bo2_vpx_decode",
                           0x832d60: "_bo2_snd_ready",
                           0x6f5600: "_bo2_snd_s16tof", 0x8336f5: "_bo2_snd_addps", 0x833acd: "_bo2_snd_biquad",
                           0x459800: "_bo2_cull_mp", 0x459ce0: "_bo2_portail_mp", 0x45a150: "_bo2_portail_racine_mp",
                           0x59a120: "_bo2_regles_find",
                           0x47ab10: "_bo2_skin1", 0x47af70: "_bo2_skin2", 0x47b4f0: "_bo2_skin3", 0x47bba0: "_bo2_skin4"}),
    "zms": dict(exe="t6zm.exe", path=os.path.expanduser("~/.bo2-native/steam/t6zm.exe.unpacked.exe"),
                out="bin/ghidra-out-zms", entry=0xa2bcfc, eh={}, longjmp=0xa25670,
                redirects={0xa20e40: "_bo2_memcpy", 0xa1fe70: "_bo2_memset",
                           0xa21938: "_bo2_libm_atan2", 0x966b40: "_bo2_vpx_decode",
                           0x833d40: "_bo2_snd_ready",
                           0x6f65d0: "_bo2_snd_s16tof", 0x8346d5: "_bo2_snd_addps", 0x834aad: "_bo2_snd_biquad",
                           0x4584c0: "_bo2_cull_zm",
                           0x59b6c0: "_bo2_regles_find",
                           0x47acf0: "_bo2_skin1", 0x47b150: "_bo2_skin2", 0x47b6d0: "_bo2_skin3", 0x47bd80: "_bo2_skin4"}),
}
if TARGET not in _TARGETS:
    raise SystemExit(f"BO2_TARGET must be one of {sorted(_TARGETS)}, got {TARGET!r}")
_T = _TARGETS[TARGET]

BIN = _T.get("path") or os.path.join(_GAME, _T["exe"])
CALLTREE_CSV = os.path.join(HERE, "..", _T["out"], "calltree.csv")

ENTRY_VA = _T["entry"]

# EH-probe correct-value stubs (Phase 1).
# The earliest CRT/security init runs two C++-EH / int3 self-test probes that
# throw an exception caught in their own frame. We can't dispatch guest C++ EH
# yet (lifted functions can't re-enter at an interior PC), so replace the two
# probe functions with hand-written stubs reproducing their observable contract.
# Phase 0 proved this is side-effect-free: the only globals they touch are two
# lazy EH-internal singletons, and the catch path writes no data globals.
# va -> EAX value on the caught path (non-load-bearing; sub_8ce740's is checked
# by `cmp eax,1` to gate the side-effect-free sub_8ce7b0, sub_8ce7b0's is
# ignored). Both stubs must also pop the caller-pushed return address (ESP+=4),
# since __remill_function_return is a no-op and the real `ret` does it inline.
EH_PROBE_STUBS = _T["eh"]     # per-target; see the BO2_TARGET block above
# VAs the shim defines itself that collide with a real function on this target.
SHIM_DEFINED = _T.get("shim_defined", ())
# Functions whose _sub_<va> is provided by the SHIM (win32_runtime.cpp) instead
# of a lifted body or a fatal stub — for remill "monsters" (OOM on lift) that
# have a simple, hand-writable contract. We emit NO stub here (leave the symbol
# undefined) so the shim's strong definition resolves the callers' `bl _sub_<va>`.
#   0x613a20: recursive glob/wildcard matcher (* ? case-insensitive) — remill OOMs
#   0x4595f0: zlib inflate(z_stream*, flush) — lifted state machine returns
#             Z_DATA_ERROR on fastfile deflate that decompresses cleanly offline;
#             overridden with host zlib (raw deflate, matches inflateInit2(-15)).
SHIM_OVERRIDE_STUBS = {0x613a20, 0x4595f0, 0x435d30}

# ⚠️ MÊME CONTRAT, SOURCE DYNAMIQUE — ET C'EST CE QUI A CASSÉ LE RE-LIFT DU
# 2026-09-15. LIFT_EXCLUDE_FILE retire le CORPS lifté d'une VA que le lien
# fait fournir par un autre des 271 objets. Mais le générateur de stubs, lui,
# revoyait cette VA « référencée et non définie » et lui écrivait un
# `define @sub_<va>` fatal : lifted.o redéfinissait donc exactement ce que
# l'exclusion venait de lui retirer, et install_lift.sh refusait l'objet sur
# 7 collisions (0x4349c0 0x447810 0x52b4f0 0x855670 0x9b0900 0xa235cf 0xa24140).
# L'exclusion doit valoir pour les DEUX étapes, corps ET stub. Rempli à la
# lecture du fichier ; vide si LIFT_EXCLUDE_FILE n'est pas donné.
LIFT_EXCLUDED = set()
STATE_EAX_OFF = 2216   # offsetof(State, gpr.rax.dword), sizeof(State)=3504
STATE_ESP_OFF = 2312   # offsetof(State, gpr.rsp.dword)


import math

def _fp80_hex_to_double(m):
    """Convert an LLVM `0xK` x86_fp80 hex literal (20 hex digits: 16-bit
    sign+exponent, then a 64-bit explicit-integer-bit mantissa) to the LLVM
    `0x` double literal (raw IEEE-754 bits) nearest to it."""
    h = m.group(0)[3:]              # drop "0xK"
    se = int(h[:4], 16)            # sign(1) + exponent(15)
    mant = int(h[4:], 16)          # 64-bit significand (leading integer bit set)
    sign = (se >> 15) & 1
    exp = se & 0x7FFF
    if exp == 0x7FFF:              # inf / nan
        val = math.nan if (mant & 0x7FFFFFFFFFFFFFFF) else math.inf
    else:
        # value = mant * 2^(exp - bias - 63); bias = 16383. ldexp never overflows
        # to an exception (returns +/-inf), and float(mant) rounds the 64-bit
        # significand to 53 bits exactly as an fp80->double downcast would.
        val = math.ldexp(float(mant), exp - 16383 - 63)
    if sign:
        val = -val
    bits = struct.unpack("<Q", struct.pack("<d", val))[0]
    return f"0x{bits:016X}"

def fix_ir_text(ir_text):
    # remill lowers cpuid/rdtsc/rdtscp/xgetbv/rd|wrmsr to __remill_sync_hyper_call,
    # but emits that ONE intrinsic's symbol name as 24 zero bytes (== the byte
    # length of "__remill_sync_hyper_call"). llvm-dis round-trips the NULs; llvm-as
    # rejects ANY NUL in a symbol name ("expected value token"), so the whole
    # function was silently dropped from the lift and became a [STUB-CALL] abort
    # (e.g. 0xa22900's cpuid). Restore the real name; the shim defines the symbol.
    ir_text = ir_text.replace('@"' + '\\00' * 24 + '"', '@__remill_sync_hyper_call')

    # Undo the self-recursion tag (see patch_intra_calls): rename every tagged
    # @sub_<target+TAG> back to @sub_<target>. Longest-first is unnecessary (all
    # tags are 8 hex digits) but the \b keeps it a whole-symbol match.
    for h in set(re.findall(r'@sub_([0-9a-fA-F]+)\b', ir_text)):
        v = int(h, 16)
        if v >= SELF_CALL_TAG:
            ir_text = re.sub(r'@sub_' + h + r'\b', f'@sub_{v - SELF_CALL_TAG:x}', ir_text)

    ir_text = ir_text.replace("x86_fp80", "double")
    ir_text = ir_text.replace("i80", "i64")
    ir_text = ir_text.replace("llvm.fabs.f80", "llvm.fabs.f64")
    ir_text = ir_text.replace("@sinl", "@sin")
    ir_text = ir_text.replace("@cosl", "@cos")
    # 80-bit x86_fp80 hex constants (0xK + 20 hex digits) can't ride along as
    # `double` operands. CONVERT each to the equivalent IEEE-754 double bit
    # pattern — the old blanket rewrite to 0.0 silently zeroed EVERY fp80
    # immediate, which corrupted the FISTP/FIST overflow threshold (2^63 -> 0),
    # so every positive x87 float->int conversion returned INT64_MIN (low dword
    # 0). That produced a phantom divide-by-zero in `(int)float` idioms.
    ir_text = re.sub(r'0xK[0-9a-fA-F]{20}', _fp80_hex_to_double, ir_text)
    lines = ir_text.split("\n")
    new_lines = []
    seen_decls = set()
    # Symbols DEFINED in this module: after the tag rename above, a self-recursive
    # call's leftover `declare @sub_<self>` collides with `define @sub_<self>`
    # (llvm-as "redefinition"). Drop such declares — the define supplies the body.
    defined_syms = set(re.findall(r'^\s*define\b[^@]*(@[\w.$]+)\s*\(', ir_text, re.M))
    for line in lines:
        m = re.match(r'(\s*)(%\w+)\s*=\s*(fptrunc|fpext)\s+double\s+(%\w+)\s+to\s+double', line)
        if m:
            line = f"{m.group(1)}{m.group(2)} = bitcast double {m.group(4)} to double"
        # De-dupe duplicate function declarations. The x86_fp80->double and
        # llvm.fabs.f80->llvm.fabs.f64 rewrites above collapse a distinct
        # `declare x86_fp80 @llvm.fabs.f80(x86_fp80)` into a SECOND
        # `declare double @llvm.fabs.f64(double)`, which llvm-as rejects as an
        # "invalid redefinition" (22 of the 27 post-remill frontier fails). The
        # call sites were rewritten identically, so keeping the first declare of
        # each symbol is correct.
        d = re.match(r'\s*declare\b.*?(@(?:"[^"]*"|[\w.$]+))\s*\(', line)
        if d:
            nm = d.group(1)
            if nm in seen_decls or nm in defined_syms:
                continue
            seen_decls.add(nm)
        new_lines.append(line)
    out = "\n".join(new_lines)
    if os.environ.get("BO2_INLINE_READS"):
        out = _inline_guest_reads(out)
    if os.environ.get("BO2_INLINE_FLAGS"):
        out = _inline_flag_computations(out)
    if os.environ.get("BO2_INLINE_TRIV"):
        out = _inline_trivial_intrinsics(out)
    if os.environ.get("BO2_INLINE_WRITES"):
        out = _inline_guest_writes(out)
    if os.environ.get("BO2_INLINE_READS") or os.environ.get("BO2_INLINE_WRITES"):
        out = _retarget_arm64(out)
    return out


# Les replis de lecture/ecriture calculent `bo2_g_base + a` sur 64 bits. Sous le
# datalayout i386 que remill pose (`p:32`), `opt -O2` sait qu'un pointeur tient
# sur 32 bits et REECRIT `ptrtoint→i64 / add i64 / inttoptr` en arithmetique i32 :
# la base 0x7000000000 est tronquee a 0 et l'acces part a l'adresse invitee brute
# (P663 : 9 runs sur 9 en SIGSEGV a esp-4, sub_4f5520 de C17). clang, lui, pose le
# datalayout arm64 avant d'optimiser ; il faut donc que `opt` le voie aussi. La
# disposition de %struct.State est identique sous les deux (247 champs, 3 504 o).
_ARM64_DL = "e-m:o-i64:64-i128:128-n32:64-S128"
_ARM64_TRIPLE = "arm64-apple-macosx18.0.0"


def _retarget_arm64(ir_text):
    ir_text = re.sub(r'^target datalayout = "[^"]*"', f'target datalayout = "{_ARM64_DL}"',
                     ir_text, count=1, flags=re.M)
    return re.sub(r'^target triple = "[^"]*"', f'target triple = "{_ARM64_TRIPLE}"',
                  ir_text, count=1, flags=re.M)

# ── BO2_INLINE_FLAGS=1 — replier les quatre predicats de drapeaux ─────────────
#
# MESURE (2026-09-13, sample de 10 s sur runs/long1, temps PROPRE par feuille) :
# les quatre __remill_flag_computation_* pesent 556 echantillons sur 4460 du
# thread invite, soit 12,5 % — devant les lectures (480) et juste derriere les
# ecritures (811).
#
# Et elles ne FONT rien. Leur definition entiere, dans win32_runtime.cpp :
#     [[gnu::used]] bool __remill_flag_computation_carry(bool r, ...) { return r; }
# Elles renvoient leur premier argument. Ce sont des fonctions IDENTITE.
#
# On paie donc trois choses pour rien : un appel non replie (lifted.o et
# extra_lifted.o les ont en symboles INDEFINIS — `nm -u` le confirme — donc
# aucune passe ne peut les inliner sans LTO), et surtout une signature
# VARIADIQUE, qui sur ARM64 fait passer les arguments par la pile.
#
# La substitution est SEMANTIQUEMENT EXACTE, pas une approximation : remplacer
# le resultat de l'appel par son premier argument, c'est la definition meme de
# la fonction. On l'ecrit `or i1 %a, false` plutot que de reecrire les usages
# SSA — LLVM replie `or X, false` en X, et cela evite de toucher a autre chose
# que l'instruction elle-meme.
#
# ⚠️ ECHOUE BRUYAMMENT SI LE MOTIF NE CORRESPOND PAS. Le mode de defaillance
# recurrent de ce depot est l'etape qui rate sans rien arreter : un remplacement
# a zero occurrence rendrait l'IR inchange et la mesure porterait sur un binaire
# identique, exactement le piege que le garde-fou de binaire d'angola_run.sh
# existe pour attraper. Ici on leve.
_FLAG_CALL = re.compile(
    # ⚠️ Le motif DOIT accepter les attributs que remill emet reellement — mesure
    # sur boot_cache/lift_full/func_4afa7c.bc, 198 occurrences :
    #   %22 = call zeroext i1 (i1, ...) @__remill_flag_computation_carry(
    #             i1 noundef zeroext %21, i32 noundef %17, i32 noundef %19)
    # Une premiere version sans `zeroext` en retour ni `noundef zeroext` en
    # parametre repliait 0 site sur 198 tout en passant six cas syntetiques.
    r'^(\s*%[\w.]+)\s*=\s*(?:tail\s+)?call\s+(?:(?:zeroext|signext|noundef)\s+)*'
    r'i1\s*(?:\(i1,\s*\.\.\.\)\s*)?'
    r'@__remill_flag_computation_(?:carry|zero|sign|overflow)\s*\('
    r'\s*i1\s+(?:(?:noundef|zeroext|signext|nonnull)\s+)*([^,)\s]+)'
    r'\s*(?:,[^)]*)?\)(?:\s*#\d+)?',
    re.M)
def _inline_flag_computations(ir_text):
    if "__remill_flag_computation" not in ir_text:
        return ir_text          # rien a replier dans ce module : normal
    # ⚠️ Compter les sites AVANT : un module peut ne porter que le `declare`
    # (aucun appel), et il ne faut pas confondre « rien a faire » avec « le
    # motif ne marche plus ». Un SystemExit n'est PAS une Exception : il
    # traverserait le `except Exception` de lift_one et tuerait le worker.
    want = sum(1 for l in ir_text.split("\n")
               if "@__remill_flag_computation_" in l and "declare" not in l)
    if want == 0:
        return ir_text
    out, n = _FLAG_CALL.subn(r'\1 = or i1 \2, false', ir_text)
    if n < want:
        raise SystemExit(
            f"BO2_INLINE_FLAGS: {want} site(s) d'appel et seulement {n} replie(s) "
            "— le motif IR a change. "
            "Relire un appel reel avant de relancer, ne pas continuer : un "
            "remplacement a zero occurrence donnerait un binaire identique et "
            "une mesure qui ne mesure rien.")
    print(f"[boot] BO2_INLINE_FLAGS: {n} predicats de drapeaux replies")
    return out

# ── BO2_INLINE_READS=1 — lower the guest READ intrinsics into plain loads ──────
#
# Every guest memory read in lifted code is a CALL to __remill_read_memory_N in
# the runtime: measured ~25% of the guest thread as a leaf, and a call is also an
# optimisation barrier (no register allocation across it, no CSE, presumed
# memory clobber), so 25% is a LOWER bound on the gain. Guest memory is one flat
# 4 GiB mmap — g_base + a IS the address — so the ideal read is one load.
#
# ⚠️ NOT a pure accessor; the inline body keeps both of its behaviours:
#   * g_park_pending — a destroyed guest thread retires itself from inside
#     __remill_read_memory_32 (its only guest-only entry). Kept: one load, one
#     predictable branch to the slow path.
#   * page straddle — a read within N-1 bytes of a 64 KB boundary takes the
#     runtime's byte path (which WRAPS at 4 GiB). Kept by deferring to _slow.
# 64/f64 are NOT lowered here: they go through mem_read64_ext, unread.
#
# `define internal`: the symbol stays module-local, so the runtime's exported
# __remill_read_memory_N (still used by lifted.o and by every object built
# without this flag) does not clash at link time.
_READ_KINDS = [  # (suffix, llvm type, return attr, straddle threshold)
    ("8",   "i8",    "zeroext ", 65536),   # one byte never straddles
    ("16",  "i16",   "zeroext ", 65534),
    ("32",  "i32",   "",         65532),
    ("f32", "float", "",         65532),
]
def _inline_guest_reads(ir_text):
    used = set(re.findall(r'@__remill_read_memory_(8|16|32|f32)\b', ir_text))
    if not used:
        return ir_text
    # `volatile`, and it is not belt-and-braces. A call is an optimisation
    # barrier; an inlined plain load is not, and LLVM may hoist it out of a
    # guest loop that contains no call -- the exact shape documented in
    # bo2-spin-inside-one-lifted-block, where a thread spins inside ONE lifted
    # block. Two loads are at stake and the park flag is the worse one: it is
    # polled THROUGH this intrinsic on purpose, because a guest-only loop has
    # no other poll point, so hoisting it means a destroyed thread can never
    # retire -- in precisely the loop the check exists for. On ARM64 a volatile
    # load is the same `ldr`; what it costs is CSE between repeated reads of
    # the same guest address, which lifted code does a lot. BO2_INLINE_READS=2
    # drops the volatile so that cost can be MEASURED rather than assumed; it
    # is not safe to ship.
    vol = "" if os.environ.get("BO2_INLINE_READS") == "2" else "volatile "
    defs = ["@bo2_g_base = external global ptr", "@g_park_pending = external global i32"]
    for suf, ty, ra, thr in _READ_KINDS:
        if suf not in used:
            continue
        ir_text = re.sub(r'^[^\n]*declare\b[^\n]*@__remill_read_memory_' + suf + r'\([^\n]*\n', '',
                         ir_text, flags=re.M)
        if thr < 65536:
            straddle = (f"  %off = and i32 %a, 65535\n  %str = icmp ugt i32 %off, {thr}\n"
                        f"  %slow = or i1 %park, %str\n")
        else:
            straddle = "  %slow = icmp ne i32 %pp, 0\n"
        defs.append(f"""declare dso_local {ra}{ty} @__remill_read_memory_{suf}_slow(ptr noundef, i32 noundef)
define internal {ra}{ty} @__remill_read_memory_{suf}(ptr noundef %mem, i32 noundef %a) alwaysinline nounwind {{
entry:
  %pp = load {vol}i32, ptr @g_park_pending, align 4
  %park = icmp ne i32 %pp, 0
{straddle}  br i1 %slow, label %S, label %F
F:
  %b = load ptr, ptr @bo2_g_base, align 8
  %bi = ptrtoint ptr %b to i64
  %az = zext i32 %a to i64
  %pi = add i64 %bi, %az
  %p = inttoptr i64 %pi to ptr
  %v = load {vol}{ty}, ptr %p, align 1
  ret {ty} %v
S:
  %r = call {ty} @__remill_read_memory_{suf}_slow(ptr %mem, i32 %a)
  ret {ty} %r
}}""")
    return ir_text + "\n\n" + "\n".join(defs) + "\n"


# ── BO2_INLINE_TRIV=1 — replier les DEUX AUTRES familles d'appels inertes ────
#
# `BO2_INLINE_FLAGS` a replie les 4 __remill_flag_computation_*, qui sont des
# fonctions IDENTITE. Recensement sur trois lots de lift (batch_0/50/120), il en
# reste deux familles exactement aussi inertes, et personne ne les avait comptees :
#
#   __remill_compare_eq/neq/uge/ule/ult/ugt/sgt/sge/slt/sle  ~1 866 sites
#     [[gnu::used]] bool __remill_compare_eq(bool r) { return r; }   <- identite
#   __remill_undefined_8/16/32/64/f32/f64                    ~2 500 sites
#     [[gnu::used]] uint8_t __remill_undefined_8(void) { return 0; } <- constante
#
# Soit ~4 400 sites contre 13 504 pour les drapeaux : un tiers de plus du meme
# gain, pour le meme prix.
#
# ⚠️ ON N'UTILISE PAS DE REGEX SUR LES SITES D'APPEL ICI, contrairement aux
# drapeaux. Les drapeaux sont VARIADIQUES (`i1 (i1, ...)`), et LLVM refuse
# d'inliner une fonction variadique — d'ou la reecriture textuelle, et d'ou les
# « 0 site replie sur 198 » de la premiere version du motif. Ces deux familles-ci
# ne sont pas variadiques : on supprime le `declare` et on DEFINIT le corps en
# `internal alwaysinline`. `opt -O2` fait le reste, et le repli ne peut pas rater
# en silence — un `declare` supprime sans `define` ne se lie meme pas.
_CMP = ["eq", "neq", "ugt", "uge", "ult", "ule", "sgt", "sge", "slt", "sle"]
_UNDEF = [("8", "i8", "zeroext ", "0"), ("16", "i16", "zeroext ", "0"),
          ("32", "i32", "", "0"), ("64", "i64", "", "0"),
          ("f32", "float", "", "0.000000e+00"), ("f64", "double", "", "0.000000e+00"),
          ("f80", "double", "", "0.000000e+00")]   # f80 -> double deja fait par fix_ir_text
# C26 (2026-09-26) — les stubs FPU du runtime (win32_runtime.cpp, « FPU exception
# stubs » et « FPU rounding mode ») sont vides ou constants, comme les familles
# ci-dessus. Declares `noinline optnone` sans attribut memoire, ils sont des
# barrieres completes : sub_8fc8f0 (x87) en porte 274 paires test/clear, et LLVM
# doit reecrire les 8 fentes x87 de l'etat (2 872 memcpy de 10 octets) a chacune.
# (nom, type rendu, parametres nommes, corps) — corps = celui du runtime, verbatim.
_FPU = [("__remill_fpu_exception_clear", "void", "i32 noundef %m", "ret void"),
        ("__remill_fpu_exception_test", "i32", "i32 noundef %m", "ret i32 0"),
        ("__remill_fpu_get_rounding", "i32", "", "ret i32 0"),
        ("__remill_fpu_set_rounding", "void", "i32 noundef %m", "ret void")]
def _inline_trivial_intrinsics(ir_text):
    defs = []
    for c in _CMP:
        if f"@__remill_compare_{c}" not in ir_text:
            continue
        n_before = ir_text.count(f"@__remill_compare_{c}")
        ir_text = re.sub(r'^[^\n]*declare\b[^\n]*@__remill_compare_' + c + r'\([^\n]*\n',
                         '', ir_text, flags=re.M)
        if ir_text.count(f"@__remill_compare_{c}") == n_before:
            raise SystemExit(f"BO2_INLINE_TRIV: aucun `declare @__remill_compare_{c}` "
                             "supprime alors que le symbole est reference — le motif "
                             "a change, ne pas continuer (un module inchange se mesure "
                             "comme un succes).")
        defs.append(f"define internal zeroext i1 @__remill_compare_{c}"
                    f"(i1 noundef zeroext %r) alwaysinline nounwind {{\n  ret i1 %r\n}}")
    for suf, ty, ra, zero in _UNDEF:
        if f"@__remill_undefined_{suf}" not in ir_text:
            continue
        n_before = ir_text.count(f"@__remill_undefined_{suf}")
        ir_text = re.sub(r'^[^\n]*declare\b[^\n]*@__remill_undefined_' + suf + r'\([^\n]*\n',
                         '', ir_text, flags=re.M)
        if ir_text.count(f"@__remill_undefined_{suf}") == n_before:
            raise SystemExit(f"BO2_INLINE_TRIV: aucun `declare @__remill_undefined_{suf}` "
                             "supprime alors que le symbole est reference.")
        defs.append(f"define internal {ra}{ty} @__remill_undefined_{suf}"
                    f"() alwaysinline nounwind {{\n  ret {ty} {zero}\n}}")
    for nom, ty, params, corps in _FPU:
        if f"@{nom}(" not in ir_text:
            continue
        n_before = ir_text.count(f"@{nom}(")
        ir_text = re.sub(r'^[^\n]*declare\b[^\n]*@' + nom + r'\([^\n]*\n', '', ir_text, flags=re.M)
        if ir_text.count(f"@{nom}(") == n_before:
            raise SystemExit(f"BO2_INLINE_TRIV: aucun `declare @{nom}` supprime alors que "
                             "le symbole est reference.")
        defs.append(f"define internal {ty} @{nom}({params}) alwaysinline nounwind {{\n  {corps}\n}}")
    if not defs:
        return ir_text
    return ir_text + "\n\n" + "\n".join(defs) + "\n"

# ── BO2_INLINE_WRITES=1 — replier la barriere d'ecriture dans le code leve ────
#
# La barriere d'ecriture est le plus gros poste du coeur invite : 43-47 % selon
# le profil en tete de win32_runtime.cpp (23,2 % win32_mem_write32 + 15,4 %
# __remill_write_memory_f32 + 8,2 % __remill_write_memory_32), et 7 308 + 1 116
# sites d'appel statiques sur trois lots de lift, contre 4 331 + 1 781 pour les
# lectures. C'est aussi le poste que `BO2_INLINE_READS` ne touche pas.
#
# La voie rapide C a DEJA ete reduite a « un mot, une charge de granule, un
# branchement predit » (voir le commentaire de win32_mem_write32). Ce qui reste
# a payer n'est donc plus le prologue : c'est L'APPEL lui-meme — barriere
# d'optimisation, aucun registre garde a travers, clobber memoire presume.
#
# ⚠️ CE QUI EST REPRODUIT ICI, ET POURQUOI CHAQUE MORCEAU Y EST :
#   * `++g_guest_writes` — NON, ce n'est pas de l'instrumentation. cas_spin_note
#     s'en sert pour distinguer un verrou tournant d'un try-lock qui travaille ;
#     l'omettre a deja tue angola en plein chargement (runs/armed1).
#   * `g_w32_armed | g_w32_keep` — un seul mot exporte, `bo2_w32_cold`, pour ne
#     pas payer deux charges. g_w32_keep N'EST PAS une source de g_w32_armed
#     (verifie dans w32_armed_sources), donc il faut le porter explicitement.
#   * la carte de granule `g_w32_slow[a >> 16]` — c'est TOUTE la surete du
#     dispositif : un octet par 64 Kio, et toute adresse speciale tombe dans un
#     granule marque, qui part sur la voie complete, verbatim.
#   * le chevauchement de page (`(a & 0xffff) > 0xfffc`) — la voie complete
#     ecrit octet par octet dans ce cas.
#   * le tic de preemption — `BO2_PREEMPT_W=20000` est un DEFAUT et c'est le
#     SEUL point de cede d'une boucle invitee sans import. Le sauter, c'est
#     rendre l'ordonnanceur non preemptif : 2/3 au spawn au lieu de 4/4.
#     ⚠️ il est pose dans `__remill_write_memory_32` et PAS dans
#     win32_mem_write32, parce que le point d'entree __remill_ n'est atteint que
#     depuis le code leve — ceder depuis un shim hote qui tient un std::mutex
#     non recursif est un interblocage franc. Le repliage garde exactement cette
#     frontiere : seul le code leve execute ce corps.
#   * `__remill_write_memory_f32` ne fait PAS le tic de preemption. Le repliage
#     non plus. Bit pour bit.
#
# 64 / f64 ne sont pas replies : ils passent par mem_write64_ext, 753 + 15 sites.
_WRITE_KINDS = [  # (suffixe, type llvm, preemption)
    ("32",  "i32",   True),
    ("f32", "float", False),
]
def _inline_guest_writes(ir_text):
    used = set(re.findall(r'@__remill_write_memory_(32|f32)\b', ir_text))
    if not used:
        return ir_text
    defs = ["@bo2_g_base = external global ptr",
            "@bo2_w32_cold = external global i32",
            "@bo2_w32_slow_map = external global ptr",
            "@g_guest_writes = external global i64",
            "@g_w32_preempt_every = external global i64",
            "@g_w32_preempt_left = external global i64",
            "@g_any_hosted = external global i8",
            "@bo2_sched_tp = external global i64",
            "declare dso_local void @win32_thread_write_tick()"]
    for suf, ty, preempt in _WRITE_KINDS:
        if suf not in used:
            continue
        ir_text = re.sub(r'^[^\n]*declare\b[^\n]*@__remill_write_memory_' + suf + r'\([^\n]*\n',
                         '', ir_text, flags=re.M)
        if preempt:
            # 🔴 P641 — la porte de P615, qui manquait ICI. P615 (21/09) a retire
            # le fil heberge du compteur dans la voie C++ seulement ; ce repliage
            # (17/09) le laissait decrementer depuis tout fil, avec `== 0`. Deux
            # coeurs : l'un ecrit 0 puis `every`, l'autre lit 0 entre les deux et
            # ecrit -1 apres -> le compteur ne revaut plus jamais 0, la preemption
            # meurt pour tout le tourniquet (P640a : 3/3 morts au frontend).
            # Qui est le fil ordonnanceur, SANS acces TLS (un `_tlv_get_addr` par
            # ecriture, cf. kernel32_impl.cpp P628/E4) : `tpidrro_el0`, le
            # registre que lit pthread_self() sur Darwin arm64, compare au mot
            # que maybe_host() pose avant de lever g_any_hosted. En cooperatif,
            # une charge et un saut de plus. Et `<= 0` signe : un compteur pousse
            # sous zero se reamorce au lieu de mourir.
            tail = """  %pe = load i64, ptr @g_w32_preempt_every, align 8
  %pez = icmp eq i64 %pe, 0
  br i1 %pez, label %D, label %H
H:
  %ah = load i8, ptr @g_any_hosted, align 1
  %ahz = icmp eq i8 %ah, 0
  br i1 %ahz, label %P, label %HT
HT:
  %tp = call i64 asm "mrs $0, tpidrro_el0", "=r"()
  %stp = load i64, ptr @bo2_sched_tp, align 8
  %me = icmp eq i64 %tp, %stp
  br i1 %me, label %P, label %D
P:
  %pl = load i64, ptr @g_w32_preempt_left, align 8
  %pl1 = add i64 %pl, -1
  store i64 %pl1, ptr @g_w32_preempt_left, align 8
  %plz = icmp slt i64 %pl1, 1
  br i1 %plz, label %T, label %D
T:
  store i64 %pe, ptr @g_w32_preempt_left, align 8
  call void @win32_thread_write_tick()
  br label %D
D:
  ret ptr %mem
"""
        else:
            tail = "  br label %D\nD:\n  ret ptr %mem\n"
        defs.append(f"""declare dso_local ptr @__remill_write_memory_{suf}_slow(ptr noundef, i32 noundef, {ty} noundef)
define internal ptr @__remill_write_memory_{suf}(ptr noundef %mem, i32 noundef %a, {ty} noundef %v) alwaysinline nounwind {{
entry:
  %c = load volatile i32, ptr @bo2_w32_cold, align 4
  %cnz = icmp ne i32 %c, 0
  %gi = lshr i32 %a, 16
  %giz = zext i32 %gi to i64
  %map = load ptr, ptr @bo2_w32_slow_map, align 8
  %gp = getelementptr i8, ptr %map, i64 %giz
  %g = load i8, ptr %gp, align 1
  %gnz = icmp ne i8 %g, 0
  %off = and i32 %a, 65535
  %str = icmp ugt i32 %off, 65532
  %s1 = or i1 %cnz, %gnz
  %slow = or i1 %s1, %str
  br i1 %slow, label %S, label %F
F:
  %w = load i64, ptr @g_guest_writes, align 8
  %w1 = add i64 %w, 1
  store i64 %w1, ptr @g_guest_writes, align 8
  %b = load ptr, ptr @bo2_g_base, align 8
  %bi = ptrtoint ptr %b to i64
  %az = zext i32 %a to i64
  %pi = add i64 %bi, %az
  %p = inttoptr i64 %pi to ptr
  store volatile {ty} %v, ptr %p, align 1
{tail}S:
  %r = call ptr @__remill_write_memory_{suf}_slow(ptr %mem, i32 %a, {ty} %v)
  ret ptr %r
}}""")
    # Une fonction qui lit ET ecrit a deja recu `@bo2_g_base` du repli des lectures ;
    # le redeclarer fait echouer llvm-as (« redefinition of global »), 26/37 sur C17.
    defs = [d for d in defs if not (d.startswith("@") and re.search(
        r'^' + re.escape(d.split(" =")[0]) + r' = ', ir_text, flags=re.M))]
    return ir_text + "\n\n" + "\n".join(defs) + "\n"


# ── C38 (2026-09-27) — REPLIER AUSSI LES LARGEURS 8, 16 ET 64 ─────────────────
#
# Profil P679 (xctrace, PC exact) : `__remill_write_memory_64` = 2,5 % de TOUT le
# CPU, appele a 93 % par le depouillement (sub_7077d0/707d50/708400_impl, 707370,
# 8fb200), et `__remill_write_memory_16` = 0,9 % (le mixeur). Le PC le plus chaud
# de sub_7077d0_impl (9,3 %), `ldr x2, [x19, #0xa38]`, n'est PAS du a l'appel : c'est
# un rangement bloque (le registre MMX vient d'etre range en voies i16/i32, puis
# relu en i64 ; le M1 ne fait pas suivre). Remede : C40 (load-store-vectorizer).
#
# CE QUI EST REPRODUIT, A L'IDENTIQUE DE win32_runtime.cpp (w8_fast_ok, w64_try_fast,
# mem_read64_ext) :
#   * ecritures : `bo2_w32_cold | granule | mot propre a la largeur` — bo2_w8_cold
#     (8/16 : BO2_W8_FAST, sondes mt, BO2_MEMFAST=0) ou bo2_w64_cold (64 : BO2_MEMFAST=0).
#     Les deux valent ~0 avant la premiere entree lente, qui les resout : le code
#     replie part donc sur la voie complete tant que [MEMFAST] n'a pas parle.
#   * test de page a la largeur (8 : aucun, un octet ne chevauche pas).
#   * `g_guest_writes` += 1 (8), 2 (16, 64, f64) : PORTEUR (cas_spin_note).
#   * AUCUN tic de preemption : les entrees 8/16/64 n'en font pas.
#   * lectures 64/f64 : mem_read64_ext = deux lectures 32 bits simples ; seule la page
#     chevauchee (deux granules, repli a 4 Gio) part sur la voie lente.
# Tout le reste tombe sur `__remill_*_slow`, qui rappelle l'entree complete verbatim.
# 80 bits (x87) : non replies. `volatile` comme les largeurs 32 (meme raison).
_WIDE_WRITES = [  # (suffixe, type llvm, mot froid propre, largeur en octets, increment du compteur)
    ("8",   "i8",     "bo2_w8_cold",  1, 1),
    ("16",  "i16",    "bo2_w8_cold",  2, 2),
    ("64",  "i64",    "bo2_w64_cold", 8, 2),
    ("f64", "double", "bo2_w64_cold", 8, 2),
]
_WIDE_READS = [  # (suffixe, type llvm)
    ("64",  "i64"),
    ("f64", "double"),
]
def _inline_guest_wide(ir_text):
    defs = ["@bo2_g_base = external global ptr",
            "@bo2_w32_cold = external global i32",
            "@bo2_w8_cold = external global i32",
            "@bo2_w64_cold = external global i32",
            "@bo2_w32_slow_map = external global ptr",
            "@g_guest_writes = external global i64"]
    n = 0
    addr = """  %b = load ptr, ptr @bo2_g_base, align 8
  %bi = ptrtoint ptr %b to i64
  %az = zext i32 %a to i64
  %pi = add i64 %bi, %az
  %p = inttoptr i64 %pi to ptr
"""
    for suf, ty, froid, nb, inc in _WIDE_WRITES:
        nom = "@__remill_write_memory_" + suf
        if not re.search(re.escape(nom) + r'\b', ir_text) or re.search(r'^define\b[^\n]*' + re.escape(nom) + r'\(', ir_text, flags=re.M):
            continue
        ir_text = re.sub(r'^[^\n]*declare\b[^\n]*' + re.escape(nom) + r'\([^\n]*\n', '', ir_text, flags=re.M)
        page = (f"  %off = and i32 %a, 65535\n  %str = icmp ugt i32 %off, {65536 - nb}\n"
                f"  %slow = or i1 %s1, %str\n") if nb > 1 else "  %slow = or i1 %s1, false\n"
        defs.append(f"""declare dso_local ptr {nom}_slow(ptr noundef, i32 noundef, {ty} noundef)
define internal ptr {nom}(ptr noundef %mem, i32 noundef %a, {ty} noundef %v) alwaysinline nounwind {{
entry:
  %c = load volatile i32, ptr @bo2_w32_cold, align 4
  %k = load i32, ptr @{froid}, align 4
  %ck = or i32 %c, %k
  %cnz = icmp ne i32 %ck, 0
  %gi = lshr i32 %a, 16
  %giz = zext i32 %gi to i64
  %map = load ptr, ptr @bo2_w32_slow_map, align 8
  %gp = getelementptr i8, ptr %map, i64 %giz
  %g = load i8, ptr %gp, align 1
  %gnz = icmp ne i8 %g, 0
  %s1 = or i1 %cnz, %gnz
{page}  br i1 %slow, label %S, label %F
F:
  %w = load i64, ptr @g_guest_writes, align 8
  %w1 = add i64 %w, {inc}
  store i64 %w1, ptr @g_guest_writes, align 8
{addr}  store volatile {ty} %v, ptr %p, align 1
  ret ptr %mem
S:
  %r = call ptr {nom}_slow(ptr %mem, i32 %a, {ty} %v)
  ret ptr %r
}}""")
        n += 1
    for suf, ty in _WIDE_READS:
        nom = "@__remill_read_memory_" + suf
        if not re.search(re.escape(nom) + r'\b', ir_text) or re.search(r'^define\b[^\n]*' + re.escape(nom) + r'\(', ir_text, flags=re.M):
            continue
        ir_text = re.sub(r'^[^\n]*declare\b[^\n]*' + re.escape(nom) + r'\([^\n]*\n', '', ir_text, flags=re.M)
        defs.append(f"""declare dso_local {ty} {nom}_slow(ptr noundef, i32 noundef)
define internal {ty} {nom}(ptr noundef %mem, i32 noundef %a) alwaysinline nounwind {{
entry:
  %off = and i32 %a, 65535
  %str = icmp ugt i32 %off, 65528
  br i1 %str, label %S, label %F
F:
{addr}  %v = load volatile {ty}, ptr %p, align 1
  ret {ty} %v
S:
  %r = call {ty} {nom}_slow(ptr %mem, i32 %a)
  ret {ty} %r
}}""")
        n += 1
    if n == 0:
        return ir_text
    defs = [d for d in defs if not (d.startswith("@") and re.search(
        r'^' + re.escape(d.split(" =")[0]) + r' = ', ir_text, flags=re.M))]
    return ir_text + "\n\n" + "\n".join(defs) + "\n"


# ── C41 (2026-09-27) — REPLIER LE `ret` INVITE ────────────────────────────────
#
# Profil P680 (xctrace 013120) : `__remill_function_return` = 2,1 % de TOUT le CPU.
# Chaque `ret` invite l'appelait pour des sondes eteintes (BO2_EBP_WATCH, BO2_RET_RING)
# et pour ranger son adresse dans une globale (fil ordonnanceur) ou un TLS (fils
# heberges, P644), que seule la remontee apres un longjmp relit. L'adresse va
# desormais dans `ArchState::_0` (offset 4, bourrage qu'aucune semantique n'ecrit),
# propre au fil par construction, et l'entree complete n'est plus appelee que si :
#   * bo2_ret_cold != 0 (sondes armees, BO2_RET_SLOW=1, ou ~0 = pas encore resolu :
#     le tout premier `ret` passe donc par l'entree complete, qui resout le mot) ;
#   * esp hors de [0x10000, 0xfff00000) ([RET-WILD-ESP], meme fenetre) ;
#   * la cible vaut 0x883440 ([RET-TO-883440]).
# esp : offset 0x908 (2312) de la State. Tout le reste tombe sur
# `__remill_function_return_slow`, qui rappelle l'entree complete verbatim.
def _inline_guest_ret(ir_text):
    nom = "@__remill_function_return"
    if not re.search(re.escape(nom) + r'\b', ir_text) or re.search(r'^define\b[^\n]*' + re.escape(nom) + r'\(', ir_text, flags=re.M):
        return ir_text
    ir_text = re.sub(r'^[^\n]*declare\b[^\n]*' + re.escape(nom) + r'\([^\n]*\n', '', ir_text, flags=re.M)
    defs = ["@bo2_ret_cold = external global i32",
            "declare i1 @llvm.expect.i1(i1, i1)",
            f"""declare dso_local ptr {nom}_slow(ptr noundef, i32 noundef, ptr noundef)
define internal ptr {nom}(ptr noundef %state, i32 noundef %addr, ptr noundef %mem) alwaysinline nounwind {{
entry:
  %rp = getelementptr inbounds i8, ptr %state, i64 4
  store i32 %addr, ptr %rp, align 4
  %sp = getelementptr inbounds i8, ptr %state, i64 2312
  %esp = load i32, ptr %sp, align 4
  %k = load i32, ptr @bo2_ret_cold, align 4
  %kn = icmp ne i32 %k, 0
  %e = add i32 %esp, -65536
  %sauv = icmp uge i32 %e, 4293853184
  %t = icmp eq i32 %addr, 8926272
  %s1 = or i1 %kn, %sauv
  %slow = or i1 %s1, %t
  %x = call i1 @llvm.expect.i1(i1 %slow, i1 false)
  br i1 %x, label %S, label %F
F:
  ret ptr %mem
S:
  %r = tail call ptr {nom}_slow(ptr %state, i32 %addr, ptr %mem)
  ret ptr %r
}}"""]
    defs = [d for d in defs if not (d.startswith("@") and re.search(
        r'^' + re.escape(d.split(" =")[0]) + r' = ', ir_text, flags=re.M))]
    if re.search(r'^declare\b[^\n]*@llvm\.expect\.i1\(', ir_text, flags=re.M):
        defs = [d for d in defs if not d.startswith("declare i1 @llvm.expect.i1")]
    return ir_text + "\n\n" + "\n".join(defs) + "\n"


REMILL_TIMEOUT_S = int(os.environ.get("REMILL_TIMEOUT_S", "30"))
REMILL_MAX_RSS_MB = int(os.environ.get("REMILL_MAX_RSS_MB", "3072"))


def _rss_mb(pid):
    try:
        r = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                           capture_output=True, text=True, timeout=5)
        return int(r.stdout.strip() or 0) // 1024
    except Exception:
        return 0


# remill prints its module's datalayout as is; LLVM 17's bitcode reader upgrades it (x86 address spaces, f80:128),
# so llvm-dis of remill's .bc printed the second form. opt must keep seeing that one (P1107).
_DL_REMILL = 'target datalayout = "e-m:x-p:32:32-i64:64-f80:32-n8:16:32-a:0:32-S32"'
_DL_BITCODE = 'target datalayout = "e-m:x-p:32:32-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32-a:0:32-S32"'


_SERVE = [None, None]   # [owner pid, Popen of `remill-lift --serve`], one per worker process (P1107)


def _remill_server():
    """This process's `remill-lift --serve` (tools/remill-patches/0005), or None if the lifter predates it."""
    if _SERVE[0] != os.getpid() or (_SERVE[1] and _SERVE[1].poll() is not None):
        h = subprocess.run([REMILL, "--help"], capture_output=True, text=True)
        srv = None
        if "-serve" in h.stdout + h.stderr:
            srv = subprocess.Popen([REMILL, "--arch", "x86", "--os", "windows", "--serve"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            srv.buf = b""
        _SERVE[:] = [os.getpid(), srv]
    return _SERVE[1]


def _server_line(srv, timeout):
    """Next line from the server, None if none came within `timeout` s. Raw reads: a buffered readline could hold
    the `exit` line while select() says there is nothing left to read."""
    while b"\n" not in srv.buf:
        if not select.select([srv.stdout], [], [], timeout)[0]:
            return None
        chunk = os.read(srv.stdout.fileno(), 4096)
        if not chunk:
            raise EOFError("remill --serve exited")
        srv.buf += chunk
    line, srv.buf = srv.buf.split(b"\n", 1)
    return line.decode()


def _remill_lift(hex_bytes, va, bc_path, ll_path):
    # RSS-capped remill run. Pathological functions make remill allocate ~6 GB
    # before the 30s timeout fires; once the machine is deep in swap even the
    # SIGKILL teardown takes minutes (measured on the 07-16 serial run: 26
    # stalls of 300-2500s each = 8.9h of the 9.3h total). macOS rejects
    # RLIMIT_DATA with EINVAL, so the cap is a poll-and-kill on the child's
    # RSS: a breach is reported as TimeoutExpired so the caller's existing
    # no-runway retry/fail path applies unchanged. Functions killed by the cap
    # are the timeout-fail class anyway; genuinely slow-but-small lifts keep
    # the full time budget. stderr goes to a temp file, not a pipe, so a
    # warning-spewing remill can never block on a full pipe and look hung.
    # P1107: the lift itself runs in a child that `remill-lift --serve` forks after loading the semantics once
    # (90 of the 110 ms of a typical lift); same lift, same cap. Without that server: one remill per function.
    args = [REMILL, "--arch", "x86", "--os", "windows",
            "--bytes", hex_bytes, "--address", hex(va),
            "--bc_out", bc_path, "--ir_out", ll_path]
    with tempfile.NamedTemporaryFile(mode="w+", encoding="utf-8", errors="replace") as errf:
        srv = _remill_server()
        if srv:
            rc = -9   # server gone or fork refused: reported like a killed remill (no .fail), restarted next time
            try:
                srv.stdin.write(f"{hex_bytes}\t{va:x}\t{bc_path}\t{ll_path}\t{errf.name}\n".encode())
                srv.stdin.flush()
                pid = int(_server_line(srv, 30).split()[1])
                if pid:
                    rc = _remill_wait(args, pid, lambda: _server_line(srv, 0.05), lambda: _server_line(srv, None))
            except (EOFError, OSError, AttributeError, ValueError):
                pid = 0
            if not pid:
                srv.kill()
                _SERVE[:] = [None, None]
        else:
            proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=errf, text=True)
            def poll():
                if proc.poll() is None:
                    time.sleep(0.05)
                    return None
                return f"exit {proc.returncode}"
            rc = _remill_wait(args, proc.pid, poll, proc.wait)
        errf.seek(0)
        stderr = errf.read()
    return subprocess.CompletedProcess(args, rc, "", stderr)


def _remill_wait(args, pid, poll, reap):
    """Exit status of the lift running as `pid`; kills it past the time or RSS cap. `poll` returns `exit <n>` once
    it ended, None meanwhile (and waits a little); `reap` waits for the end after a kill."""
    deadline = time.monotonic() + REMILL_TIMEOUT_S
    next_rss_check = time.monotonic() + 0.5
    while not (status := poll()):
        now = time.monotonic()
        if now >= next_rss_check:
            next_rss_check = now + 0.5
            if _rss_mb(pid) > REMILL_MAX_RSS_MB:
                now = deadline
        if now >= deadline:
            os.kill(pid, 9)
            reap()
            raise subprocess.TimeoutExpired(args, REMILL_TIMEOUT_S)
    return int(status.split()[1])

def lift_one(args):
    va, rva, size, base_hex, runway_hex, name, workdir = args
    bc_path = os.path.join(workdir, f"func_{va:x}.bc")
    fixed_bc = bc_path + ".fixed"
    # Resume checkpoint: reuse a completed .fixed from a prior (possibly crashed)
    # run so a mid-lift crash RESUMES instead of restarting. .fixed is written
    # atomically below (tmp + os.replace), so its mere presence guarantees a
    # complete, valid artifact — safe to trust and skip.
    if os.path.exists(fixed_bc) and os.path.getsize(fixed_bc) > 0:
        return (va, fixed_bc, True, None)
    # Negative-result cache: a .fail marker means this exact function already
    # failed the full pipeline in a prior run. Without it every resume
    # re-attempts all known monsters (~30s each x ~115 = ~1h per incremental
    # frontier iteration). Set BOOT_RETRY_FAILS=1 to ignore the markers after
    # a pipeline improvement that could rescue them.
    fail_marker = bc_path + ".fail"
    if os.path.exists(fail_marker) and not os.environ.get("BOOT_RETRY_FAILS"):
        with open(fail_marker) as f:
            return (va, None, False, "cached: " + f.read().strip()[:40])

    def _fail(reason):
        with open(fail_marker, "w") as f:
            f.write(reason)
        return (va, None, False, reason)

    # BOOT_BC_SRC=<dir> — REUSE remill's output from an earlier lift instead of
    # re-running the lifter. remill's .bc is produced BEFORE fix_ir_text(), so a
    # change confined to fix_ir_text (this is what BO2_INLINE_READS is) needs only
    # the tail of the pipeline: llvm-dis -> fix -> llvm-as -> opt. That turns a
    # 1.5-2.5 h single-threaded re-lift into a re-fixup that parallelises safely
    # (the -j 1 rule exists for remill's memory appetite, and remill does not run).
    # The cached .bc is read IN PLACE, never copied: lift_wip is 2.5 GB and
    # angola_run.sh refuses to start below 8 GiB free.
    bc_src = os.environ.get("BOOT_BC_SRC")
    cached_bc = os.path.join(bc_src, f"func_{va:x}.bc") if bc_src else None
    reuse = bool(cached_bc and os.path.exists(cached_bc)
                 and os.path.getsize(cached_bc) > 0)
    # De-hang self-recursion: retarget intra-range direct calls out of range so
    # remill emits external @sub_ calls instead of infinitely expanding the trace.
    # fix_ir_text() renames the tagged symbols back. (Runway bytes are appended
    # UNpatched — an adjacent self-referential body dragged in via runway is
    # handled by the no-runway retry below.) Pointless when reusing: the cached
    # .bc already reflects it, and the disassembly is not free.
    if not reuse and os.environ.get("LIFT_PISTE_ENTIERE"):
        # P989 (Steam) : `call $+5 ; pop r32` n'est pas un appel (FLAC, 15 sites par exe) ; `push <suivant>` fait la meme
        # chose a la machine et ne laisse rien a mal modeliser. Seulement a un debut d'instruction (P748).
        corps = bytearray.fromhex(base_hex + runway_hex)
        for i in [a - va for a, _, _, _ in _md_patch.disasm_lite(bytes(corps), va)]:
            if corps[i:i + 5] == b"\xe8\0\0\0\0" and i + 5 < len(corps) and 0x58 <= corps[i + 5] <= 0x5f:
                corps[i:i + 5] = b"\x68" + (va + i + 5).to_bytes(4, "little")
        base_hex, runway_hex = corps[:len(base_hex) // 2].hex(), corps[len(base_hex) // 2:].hex()
    if not reuse:
        try:
            base_hex = patch_intra_calls(bytes.fromhex(base_hex), va, size)[0].hex()
        except Exception:
            pass  # patch best-effort; a disasm hiccup falls back to original bytes
    ll_path = bc_path + ".ll"
    try:
        if not reuse:
            try:
                r = _remill_lift(base_hex + runway_hex, va, bc_path, ll_path)
            except subprocess.TimeoutExpired:
                # Runway bytes can drag an adjacent or self-referential function body
                # into remill's trace lifter and make it hang. On timeout, fall back to
                # the function's own bytes — verified strictly better than degrading to
                # a timed-out inert stub (see docs/import_arity_audit sibling analysis).
                if not runway_hex:
                    raise
                r = _remill_lift(base_hex, va, bc_path, ll_path)
            if r.returncode < 0:
                # Tue par un signal (garde disque, pkill) : pas un echec du lifter. Un marqueur .fail ici
                # rendait la fonction STUB-CALL pour toujours (0xa6a3a0, 0x5585f0 en boucle).
                return (va, None, False, f"lift: signal {-r.returncode}")
            if r.returncode != 0:
                return _fail(f"lift: {r.stderr[:60]}")
        # fix IR. remill writes the text itself (--ir_out): no llvm-dis. A reused .bc still needs one.
        if reuse:
            subprocess.run([LLVM17 + "/bin/llvm-dis", "-o", ll_path, cached_bc],
                          capture_output=True, text=True, timeout=10)
        with open(ll_path) as f: ir_text = f.read()
        ir_text = ir_text.replace(_DL_REMILL, _DL_BITCODE, 1)
        # untag_self_call_pcs only rewrites a pc `add` of TAG-16M or more, i.e. a call target at or above it:
        # with none in remill's output, untag_fixed (llvm-dis, text, llvm-as) would change nothing (P1107).
        tagged = any(int(h, 16) >= SELF_CALL_TAG - 0x1000000 for h in re.findall(r'@sub_([0-9a-fA-F]+)\b', ir_text))
        with open(ll_path, "w") as f: f.write(fix_ir_text(ir_text))
        # opt parses the text itself (P1107: same .fixed, byte for byte, as llvm-as then opt) -> write
        # ATOMICALLY (tmp + os.replace) so a crash mid-write never leaves a truncated .fixed that the
        # resume-skip above would wrongly trust.
        tmp_fixed = fixed_bc + ".tmp"
        r = subprocess.run([LLVM17 + "/bin/opt", "-O2", "-o", tmp_fixed, ll_path],
                          capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            return _fail("opt")
        if tagged and untag_fixed(tmp_fixed) < 0:
            return _fail("untag")
        os.replace(tmp_fixed, fixed_bc)
        # Nothing reads the intermediates again: ~2 GB on multi (97 k functions), which filled the disk of a fresh
        # build (26/10). With reuse the .bc stays in BOOT_BC_SRC and the workdir holds only .fixed.
        if True:
            for t in (ll_path,):
                try: os.unlink(t)
                except OSError: pass
        return (va, fixed_bc, True, None)
    except Exception as e:
        return _fail(str(e)[:60])


def read_pe_sections():
    """Read all PE sections and return (va, raw_bytes) pairs."""
    sys.path.insert(0, HERE)
    from pe_parser import PEFile
    pe = PEFile(BIN).parse()
    sections = []
    # PE headers at the image base: the CRT validates them in memory ('MZ'/'PE'
    # via __ValidateImageBase / __IsNonwritableInCurrentImage). Without them
    # _cinit SKIPS the _FPinit call -> _cfltcvt_tab keeps _fptrap -> printf %f
    # aborts with R6002 "floating point support not loaded".
    hdr_size = 0x1000
    sections.append((pe.image_base, 0, hdr_size, pe._raw[:hdr_size], "hdrs"))
    for s in pe.sections:
        off = pe.rva_to_offset(s.virtual_address)
        raw = pe._raw[off:off + s.size_of_raw_data]
        va = pe.image_base + s.virtual_address
        sections.append((va, s.virtual_address, s.virtual_size, raw, s.name))
    return sections, pe


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("-n", "--count", type=int, default=2000,
                       help="functions to lift from entry chain")
    parser.add_argument("-j", "--jobs", type=int, default=4)
    parser.add_argument("--build-only", action="store_true", help="build/sign, do not start the game")
    args = parser.parse_args()

    if os.environ.get("BOOT_RELINK_ONLY"):
        # Une retouche du runtime ne change pas lifted.o : recompiler le runtime et relier (minutes, pas 55 min).
        workdir = os.environ["BOOT_WORKDIR"]
        lifted_o = os.path.join(workdir, "lifted.o")
        with open(os.path.join(workdir, "bc_files.txt")) as f:
            bc_files = f.read().split()
        print(f"Relink only: {len(bc_files)} lifted functions, {lifted_o}")
        return _finish_build(args, workdir, lifted_o, bc_files, time.time())

    print("=== BOOT ATTEMPT: lift entry chain, map PE, run entry ===")
    print(f"entry: {ENTRY_VA:#x}")
    print(f"lifting {args.count} functions from entry chain")
    print()

    # Read entry chain functions
    functions = []
    stripped_zero = []
    with open(CALLTREE_CSV) as f:
        reader = csv.DictReader(f)
        for i, row in enumerate(reader):
            if len(functions) >= args.count:
                break
            va = int(row["va"], 16)
            size = int(row["size"])
            hex_bytes = row["bytes_hex"]
            name = row["name"]
            if size < 4 or size > 100000:
                continue
            # A function whose bytes are all zero is not code. t6mp and t6zm ship
            # ~412 KB / 103 pages of .text zero-filled (Steam CEG strips them and
            # restores at runtime; t6sp on disk is already unwrapped and has NO
            # such pages). Both Ghidra and our call tree happily catalogue
            # "functions" there — 1211/22789 and 934/11949 on mp — and 245 of them
            # landed in the first-3000 slice. Lifting zeros yields a run of
            # `add [eax],al`, so it can only produce garbage, and it displaces a
            # real function from the slice.
            if hex_bytes and not hex_bytes.strip("0"):
                stripped_zero.append(va)
                continue
            functions.append((va, int(row["rva"], 16), size, hex_bytes, name))

    print(f"Read {len(functions)} functions ({sum(f[2] for f in functions)} bytes)")
    if stripped_zero:
        # Never silent: on an image whose code is still packed this number IS the diagnosis.
        print(f"  ⚠️  skipped {len(stripped_zero)} all-zero \"function(s)\" — that "
              f"region is not in the file (.text still packed); e.g. "
              f"{', '.join(hex(v) for v in stripped_zero[:4])}")

    # Runway bytes: calltree.csv function sizes sometimes stop a few bytes short of
    # the real function end (Ghidra bounds occasionally cut 1-46B before the final
    # instruction / a POGO-relocated cold block), so remill runs out of bytes and
    # emits __remill_error/__remill_missing_block at that PC and aborts at runtime
    # (e.g. __setmbcp at 0x96faae). We append real image bytes so remill can finish.
    #
    # Two safety bounds (an adversarial verification found a blanket fixed runway is
    # neither sufficient nor safe): (1) trim at the NEXT function's start so the
    # runway can never drag an adjacent or self-referential function body into the
    # trace lifter (which hung FUN_0096d065); this also lets ___tmainCRTStartup get
    # its full 46B gap. (2) clamp at the containing section's virtual end, because
    # rva_to_offset happily reads past a section into padding/next-section bytes.
    # lift_one additionally retries without runway on timeout as a backstop.
    import bisect
    from pe_parser import PEFile
    RUNWAY = 64
    # 🎯 RUNWAY_SYNTH — une etendue SYNTHETISEE peut etre coupee net, et 64 octets
    # ne suffisent alors pas de plusieurs KILO-octets.
    #
    # `tools/lift_roots.py` fabrique l'etendue d'une fonction que Ghidra n'a pas
    # identifiee : `region_end = min(next_start, va + SYNTH_CAP)` avec
    # SYNTH_CAP = 0x1000. Quand la vraie fonction est plus longue, le balayage
    # lineaire s'arrete a l'interieur, remill decode dans la piste de 64 octets,
    # manque d'octets AU MILIEU d'une instruction, et pose
    # `hyper_call = 32` (AsyncHyperCall::kInvalidInstruction) suivi de
    # `__remill_error`. A l'execution c'est un signal 6 immediat.
    #
    # MESURE (2026-09-17) : `sub_94d7b0` (SHA-256, vraie taille >= 7 064 o) est
    # coupee a 4 092 ; remill meurt a 0x94e7eb sur
    # `lea edx,[ebx+edx+0x27b70a85]`, 7 octets, dont il n'en a que 5. Le jeu
    # mourait a 5 s, 1/1. 455 fonctions sont dans ce cas.
    #
    # La liste est EXPLICITE (`tools/lift_runway_wide.txt`, produite en comparant les etendues a SYNTH_CAP=0x1000
    # et 0x8000) plutot qu'une heuristique « taille proche du plafond » : la
    # moitie des 455 s'arretent BIEN avant 4 096 (un `ret` atteint tot parce que
    # les sauts avant pointaient hors de la fenetre), donc un seuil sur la taille
    # ne les voit pas.
    #
    # ⚠️ Les DEUX bornes de surete restent en vigueur telles quelles : la piste
    # est coupee au prochain depart de fonction catalogue et a la fin de section.
    # Au-dela de 0x8000 elles saturent (339/455 couvertes a 0x4000 comme a
    # 0x10000), donc inutile d'aller plus haut.
    RUNWAY_SYNTH = 0x8000
    _wide_file = os.environ.get("LIFT_RUNWAY_FILE",
                                os.path.join(os.path.dirname(__file__), "lift_runway_wide.txt"))
    _wide = set()
    if os.path.exists(_wide_file):
        for _l in open(_wide_file):
            _l = _l.split("#")[0].strip()
            if _l:
                _wide.add(int(_l, 16))
        print(f"Runway elargie ({RUNWAY_SYNTH}B) pour {len(_wide)} etendues synthetisees coupees")
    _pe = PEFile(BIN).parse()
    csv.field_size_limit(1 << 30)
    all_starts = sorted({int(r0["va"], 16) for r0 in csv.DictReader(open(CALLTREE_CSV))})

    # --- root-seeded closure -------------------------------------------------
    # The call-tree base above is a static BFS; it can't see through the CRT's
    # indirect dispatch of its C/C++ initializers (the __xi/__xc pointer tables),
    # so those functions and their direct-call closure never get lifted and fault
    # at runtime (STATUS_INVALID_CRUNTIME_PARAMETER in _cinit). Seed the lifter
    # with those tables' targets and follow direct call/jmp edges. General by
    # design: LIFT_ROOTS_FILE can add VAs harvested from runtime
    # __remill_missing_block hits (or more tables) as we go deeper.
    from lift_roots import extra_functions_from_roots
    base_vas = {f[0] for f in functions}
    extra = extra_functions_from_roots(_pe, os.path.dirname(CALLTREE_CSV),
                                       roots_file=os.environ.get("LIFT_ROOTS_FILE"))
    added = [e for e in extra if e[0] not in base_vas]
    functions.extend(added)
    all_starts = sorted(set(all_starts) | {e[0] for e in added})
    _n_synth = sum(1 for e in added if e[4].startswith("init_"))
    print(f"Root-seeded closure: +{len(added)} functions from initializer tables "
          f"({_n_synth} synthesized, {len(added) - _n_synth} known) -> {len(functions)} total")

    # ── LIFT_EXCLUDE_FILE=<fichier> — NE PAS lifter ces VA ────────────────────
    #
    # Le lien lie lifted.o AVEC ~35 extra_lifted*.o et ~200
    # *_impl.o, et ceux-la DEFINISSENT eux aussi des `_sub_<va>` (extra_lifted.o
    # a lui seul : 3007). Aujourd'hui il n'y a pas de collision parce que la
    # liste d'entree de juillet ne les contenait pas — ce sont justement les
    # fonctions que le lift n'arrivait pas a faire et qui ont ete reprises a la
    # main. Des qu'on elargit la liste (ou que la cloture les attrape), le lien
    # echoue sur « duplicate symbol ». Ce fichier est la liste des VA fournies
    # ailleurs ; elle se calcule en une commande :
    #
    #   ls boot_cache/*.o | grep -v '/lifted.o$' | xargs nm \
    #     | awk '$2=="T" && $3 ~ /^_sub_[0-9a-f]+$/ {print substr($3,6)}' | sort -u
    #
    # ⚠️ Exclure ici est le bon sens de la correction : la version ecrite a la
    # main gagne, et une fonction retiree du lift n'est pas perdue — elle est
    # fournie par l'autre objet.
    _excl_file = os.environ.get("LIFT_EXCLUDE_FILE")
    if _excl_file:
        excl = set()
        with open(_excl_file) as f:
            for line in f:
                t = line.split('#')[0].strip()
                if t:
                    try: excl.add(int(t, 16))
                    except ValueError: pass
        LIFT_EXCLUDED.update(excl)
        before = len(functions)
        functions = [f for f in functions if f[0] not in excl]
        all_starts = sorted(set(all_starts) - excl)
        print(f"LIFT_EXCLUDE_FILE: {len(excl)} VA fournies ailleurs -> "
              f"{before - len(functions)} fonction(s) retiree(s), {len(functions)} restantes")
    secs = [(_pe.image_base + s.virtual_address,
             _pe.image_base + s.virtual_address + s.virtual_size) for s in _pe.sections]
    def _sec_end(va):
        for a, b in secs:
            if a <= va < b:
                return b
        return None
    def _img_byte(va):
        try:
            return _pe._raw[_pe.rva_to_offset(va - _pe.image_base)]
        except Exception:
            return None
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    _cs = Cs(CS_ARCH_X86, CS_MODE_32)

    def _au_milieu(va, cut):
        # Balayage linéaire depuis va : une instruction commence-t-elle avant `cut` et
        # finit-elle après ?
        code = bytearray()
        for a in range(va, cut + 15):
            b = _img_byte(a)
            if b is None:
                break
            code.append(b)
        for i in _cs.disasm_lite(code, va):
            if i[0] >= cut:
                break
            if i[0] + i[1] > cut:
                return True
        return False

    prolonges = 0
    elargies = 0
    padded = 0
    functions_run = []
    for va, rva, size, hex_bytes, name in functions:
        end = va + size
        # first cataloged function start AT-OR-AFTER this function's end. Using >=end
        # (not >va) skips any nested/overlapping starts inside [va,end) that would
        # otherwise collapse the runway to zero (regressed __setmbcp).
        ni = bisect.bisect_left(all_starts, end)
        pas = RUNWAY_SYNTH if va in _wide else RUNWAY
        limit = end + pas
        # P943 : une racine peut tomber AU MILIEU d'une instruction (0x7f304c, dans le
        # `add eax,0xcc` de 0x7f304a, fin de sub_7f2f90 que Ghidra coupe à 186 o, vraie fin
        # 0x7f3053). Couper la piste là laissait remill sans la fin de la fonction :
        # __remill_error, signal 6 dès savannah_start, en mono comme en srvbe. Une telle
        # racine n'est pas une frontière : on coupe au départ suivant.
        while ni < len(all_starts) and all_starts[ni] < limit and _au_milieu(va, all_starts[ni]):
            ni += 1
            prolonges += 1
        if ni < len(all_starts):
            limit = min(limit, all_starts[ni])            # trim at next function
        se = _sec_end(end)
        if se is not None:
            limit = min(limit, se)                        # clamp at section end
        # P986 : piste de 64 o tombée au milieu d'une instruction (0xa3af8b, coupée par le faux départ
        # catalogue 0xa3afaa) = __remill_error à l'exécution. On passe à la piste large, cache de la VA purgé.
        if pas == RUNWAY and os.environ.get("LIFT_PISTE_ENTIERE") and _au_milieu(va, limit):
            limit = end + RUNWAY_SYNTH
            ni = bisect.bisect_left(all_starts, end)
            while ni < len(all_starts) and all_starts[ni] < limit and _au_milieu(va, all_starts[ni]):
                ni += 1
            if ni < len(all_starts):
                limit = min(limit, all_starts[ni])
            if se is not None:
                limit = min(limit, se)
            elargies += 1
            for suffixe in ("", ".fixed", ".fail"):
                cache = os.path.join(os.environ["BOOT_WORKDIR"], f"func_{va:x}.bc{suffixe}")
                if os.path.exists(cache):
                    os.remove(cache)
        runway = bytearray()
        a = end
        while a < limit:
            b = _img_byte(a)
            if b is None:
                break
            runway.append(b); a += 1
        if runway:
            padded += 1
        functions_run.append((va, rva, size, hex_bytes, runway.hex(), name))
    functions = functions_run
    print(f"Appended gap-bounded runway (<= {RUNWAY}B, trimmed at next-fn/section-end) to {padded} functions")
    print(f"[PISTE] {prolonges} racine(s) au milieu d'une instruction ignoree(s) comme borne de piste")
    print(f"[PISTE] {elargies} piste(s) coupee(s) au milieu d'une instruction, elargies")

    # Persistent, resumable workdir when BOOT_WORKDIR is set (completed .fixed
    # files are reused on a re-run, so a crash mid-lift resumes). Otherwise a
    # throwaway tempdir as before.
    workdir = os.environ.get("BOOT_WORKDIR")
    if workdir:
        os.makedirs(workdir, exist_ok=True)
        print(f"Work dir (persistent/resumable): {workdir}")
        # Les .fixed d'avant untag_self_call_pcs portent encore le pc marque : une passe, une fois.
        marque = os.path.join(workdir, "untag_self_calls.done")
        if not os.path.exists(marque):
            fx = sorted(glob.glob(os.path.join(workdir, "func_*.bc.fixed")))
            with ProcessPoolExecutor(max_workers=args.jobs) as ex:
                res = list(ex.map(untag_fixed, fx, chunksize=64))
            bad = sum(1 for v in res if v < 0)
            print(f"untag_self_call_pcs : {sum(v for v in res if v > 0)} pc corriges dans "
                  f"{sum(1 for v in res if v > 0)}/{len(fx)} .fixed, {bad} echec(s)")
            if bad == 0:
                open(marque, "w").close()
    else:
        workdir = tempfile.mkdtemp(prefix="boot_")
        print(f"Work dir: {workdir}")

    # Lift
    serial = (args.jobs == 1)
    print(f"Lifting with {args.jobs} job(s){' [STRICTLY SERIAL: one remill at a time]' if serial else ' parallel'}...")
    start = time.time()
    lift_args = [(va, rva, size, base, rw, name, workdir)
                 for va, rva, size, base, rw, name in functions]
    bc_files = []
    ok = fail = 0
    if serial:
        # ONE remill at a time in THIS process — no worker pool, lowest possible
        # peak memory (a single lift's remill). Required on tight RAM.
        for i, a in enumerate(lift_args):
            va, bc, success, err = lift_one(a)
            if success:
                bc_files.append(bc); ok += 1
            else:
                fail += 1
            if (i+1) % 200 == 0 or (i+1) == len(functions):
                print(f"  [{i+1}/{len(functions)}] {ok} ok, {fail} fail, {time.time()-start:.0f}s", flush=True)
    else:
        # fork, pas spawn : sous spawn le pool se bloque apres ~32 taches (memoire bo2-boot-py-pool-hangs) ;
        with ProcessPoolExecutor(max_workers=args.jobs, mp_context=multiprocessing.get_context("fork")) as ex:
            # A few tasks per worker in flight, never all 55 000 at once: each submit() writes to the pool's wakeup
            # pipe, and once it is full the pool can freeze for good at 0 % CPU (CPython gh-105829, not fixed in 3.9;
            # issue #4: 30 functions, then nothing all night).
            todo, en_vol, i = iter(lift_args), set(), 0
            while True:
                for a in todo:
                    en_vol.add(ex.submit(lift_one, a))
                    if len(en_vol) >= 8 * args.jobs:
                        break
                if not en_vol:
                    break
                faits, en_vol = wait(en_vol, return_when=FIRST_COMPLETED)
                for fut in faits:
                    i += 1
                    va, bc, success, err = fut.result()
                    if success:
                        bc_files.append(bc); ok += 1
                    else:
                        fail += 1
                    if i % 500 == 0 or i == len(functions):
                        print(f"  [{i}/{len(functions)}] {ok} ok, {fail} fail, {time.time()-start:.0f}s")
    print(f"Lift: {ok}/{len(functions)} ({time.time()-start:.0f}s)")
    if os.environ.get("BO2_LIFT_ONLY"):   # boucle recolte -> lift jusqu'a point fixe, sans lier a chaque tour
        return

    # Phase 1: drop the EH-probe functions from the link set so the
    # correct-value stubs generated below define them instead of the lifted
    # bodies (whose in-frame throw we can't yet dispatch). Filtering by the
    # `func_<va>.bc` path segment; the `.bc.fixed` suffix still matches.
    # ⚠️ SAME PROBLEM AS THE EH PROBES, WIDER CAUSE. The shim itself defines a
    # few `sub_<t6sp va>` bodies, and on another target that address is ordinary
    # code the lift also defines — multi died with
    #     duplicate symbol '_sub_435d30' in: win32_runtime.o / lifted.o
    # A "t6sp-only" VA is only t6sp-only for the binary it came from. The shim
    # wins (it IS the interpose), so drop the lifted body.
    #
    # This list is discovered, not predicted: build, read the `duplicate symbol`
    # line, add the VA. It cannot be computed here because the shim objects are
    # not compiled until much later in this function, and grepping the sources
    # misses the `_impl` names that macros generate.
    if SHIM_DEFINED:
        _clash = sorted(set(SHIM_DEFINED) & set(all_starts))
        if _clash:
            _drop = {f"func_{va:x}.bc" for va in _clash}
            _before = len(bc_files)
            bc_files = [b for b in bc_files if not any(d in b for d in _drop)]
            print(f"  shim override: excluded {_before - len(bc_files)} lifted "
                  f"body(ies) the shim also defines: "
                  f"{', '.join(hex(v) for v in _clash[:6])}")

    if EH_PROBE_STUBS:
        _drop = {f"func_{va:x}.bc" for va in EH_PROBE_STUBS}
        before = len(bc_files)
        bc_files = [b for b in bc_files if not any(d in b for d in _drop)]
        print(f"  EH-probe override: excluded {before - len(bc_files)} lifted "
              f"body(ies) {sorted(hex(v) for v in EH_PROBE_STUBS)} from the link")

    # Link + generate stubs
    print("Linking...")
    linked_bc = os.path.join(workdir, "linked.bc")
    batch_size = 200
    batch_bcs = []
    for i in range(0, len(bc_files), batch_size):
        batch = bc_files[i:i+batch_size]
        bbc = os.path.join(workdir, f"batch_{i//batch_size}.bc")
        r = subprocess.run([LLVM17 + "/bin/llvm-link", "-o", bbc] + batch,
                          capture_output=True, text=True, timeout=120)
        if r.returncode == 0:
            batch_bcs.append(bbc)
    # Split compile (below) never reads linked.bc: the stub census reads the batches instead. Linking 489 batches
    # into one module (97 k functions) took 12 GB of swap on an 8 GB Mac (26/10).
    if len(batch_bcs) > 1 and os.environ.get("BOOT_SPLIT_COMPILE", "1") == "1":
        if os.path.exists(linked_bc):
            os.remove(linked_bc)   # an older or truncated module would be read as this lift's census
    elif len(batch_bcs) > 1:
        subprocess.run([LLVM17 + "/bin/llvm-link", "-o", linked_bc] + batch_bcs,
                      capture_output=True, text=True, timeout=1200)
    elif batch_bcs:
        import shutil; shutil.copy(batch_bcs[0], linked_bc)

    # Cache the STUB-FREE bitcode (all successfully-lifted functions, before the
    # tracing stubs are mixed in) so FUTURE targeted stub-fixes splice at the
    # bitcode level instead of a full re-lift:
    #   re-lift the N newly-fixed funcs -> llvm-link(linked_nostubs.bc, <new .fixed>)
    #   -> regen stubs (minus N) -> clang -> lifted.o    (minutes, not hours)
    # The N were STUBS here (they failed the original lift), so their fresh
    # bitcode adds with NO duplicate symbol. This makes THIS the last full
    # re-lift needed for the post-remill (fix_ir_text) failure class.
    if os.path.exists(linked_bc):
        import shutil
        _nostubs = os.path.join(HERE, "..", "boot_cache", f"linked_nostubs_{TARGET}.bc")
        os.makedirs(os.path.dirname(_nostubs), exist_ok=True)
        shutil.copy(linked_bc, _nostubs)
        print(f"  Cached stub-free bitcode -> {_nostubs} "
              f"({os.path.getsize(_nostubs)/1024/1024:.1f} MB)")

    # Find undefined functions and create tracing stubs
    print("Generating tracing stubs for undefined functions...")
    # llvm-nm, pas llvm-dis : le texte desassemble de linked.bc (185 Mo de bitcode a 22 k fonctions) pese
    # plusieurs Go en memoire Python et restait vivant pendant la compilation clang ; le swap a rempli le disque.
    # Meme ensembles : un nom `sub_<hex>[_implN]` compte pour <hex>, defini ou reference.
    def _nm_hex(flag):
        out = subprocess.run([LLVM17 + "/bin/llvm-nm", flag, "-j"] + ([linked_bc] if os.path.exists(linked_bc)
                             else batch_bcs), capture_output=True, text=True, timeout=600).stdout
        return set(re.findall(r'^_?sub_([0-9a-f]+)', out, re.M))
    defined = _nm_hex("--defined-only")
    referenced = defined | _nm_hex("--undefined-only")
    undefined = referenced - defined
    # Ensure the EH-probe overrides are always emitted (they were excluded from
    # the link above, so they are undefined; add explicitly in case a caller was
    # inlined and the reference vanished).
    undefined |= {f"{va:x}" for va in EH_PROBE_STUBS}
    # Une VA fournie par un autre objet du lien n'est ni une frontière ni un stub :
    # le linker la résout. L'émettre en stub fatal ici est un `duplicate symbol`
    # garanti (cf. LIFT_EXCLUDED plus haut), et l'écrire dans undefined_stubs.txt
    # ferait boucler lift_closure.sh sur des racines que l'exclusion re-retire.
    _provided = {h for h in undefined if int(h, 16) in LIFT_EXCLUDED}
    _provided -= {f"{va:x}" for va in EH_PROBE_STUBS}
    if _provided:
        undefined -= _provided
        print(f"  LIFT_EXCLUDE_FILE: {len(_provided)} stub(s) non émis, fournis "
              f"ailleurs dans le lien ({', '.join('0x' + h for h in sorted(_provided)[:7])})")
    print(f"  {len(defined)} defined, {len(undefined)} need stubs")

    # Dump the referenced-but-unlifted frontier (the stub set) to a stable file so
    # it can be harvested as LIFT_ROOTS_FILE for the next re-lift. This is the
    # complete, deterministic harvest (the runtime [STUB] trace is capped at 80),
    # and is exactly the indirect-dispatch frontier the static BFS can't reach.
    stub_dump = os.path.join(HERE, "..", "boot_cache", f"undefined_stubs_{TARGET}.txt")
    os.makedirs(os.path.dirname(stub_dump), exist_ok=True)
    with open(stub_dump, "w") as sf:
        for va_hex in sorted(undefined, key=lambda h: int(h, 16)):
            sf.write(f"0x{va_hex}\n")
    print(f"  Wrote {len(undefined)} stub VAs -> {stub_dump}")

    # Loud abort-on-call stubs. A referenced-but-unlifted function that is actually
    # CALLED aborts with its guest address (like [MISSING]) rather than returning a
    # no-op (%mem unchanged, eax stale) that silently corrupts gate logic — the
    # 0x4f0b20 class of bug. The abort lives INSIDE the stub body, so it only fires
    # on a real call; address-taken / relocation references that never execute the
    # body stay silent. (The trailing `ret` is dead after abort but keeps the IR
    # well-formed.)
    stub_ll = os.path.join(workdir, "stubs.ll")
    with open(stub_ll, "w") as f:
        f.write('target triple = "arm64-apple-macosx18.0.0"\n')
        f.write('declare void @__stub_call_fatal(i32)\n')
        for va_hex in sorted(undefined):
            va_int = int(va_hex, 16)
            if va_int in SHIM_OVERRIDE_STUBS:
                continue  # _sub_<va> defined in the shim; leave undefined for the linker
            if va_int in EH_PROBE_STUBS:
                # Correct-value stub for an EH self-test probe: set EAX to the
                # caught-path value and pop the caller-pushed return address
                # (ESP+=4), reproducing the observable effect of throw→catch→ret
                # without dispatching guest EH.
                eaxv = EH_PROBE_STUBS[va_int]
                f.write(f'define ptr @sub_{va_hex}(ptr noalias %state, i32 %pc, ptr noalias %mem) {{\n')
                f.write(f'  %eax = getelementptr inbounds i8, ptr %state, i32 {STATE_EAX_OFF}\n')
                f.write(f'  store i32 {eaxv}, ptr %eax, align 1\n')
                f.write(f'  %esp = getelementptr inbounds i8, ptr %state, i32 {STATE_ESP_OFF}\n')
                f.write(f'  %sp = load i32, ptr %esp, align 1\n')
                f.write(f'  %sp2 = add i32 %sp, 4\n')
                f.write(f'  store i32 %sp2, ptr %esp, align 1\n')
                f.write(f'  ret ptr %mem\n')
                f.write(f'}}\n')
                continue
            f.write(f'define ptr @sub_{va_hex}(ptr noalias %state, i32 %pc, ptr noalias %mem) {{\n')
            f.write(f'  call void @__stub_call_fatal(i32 {va_int})\n')
            f.write(f'  ret ptr %mem\n')
            f.write(f'}}\n')
    stub_bc = os.path.join(workdir, "stubs.bc")
    r_as = subprocess.run([LLVM17 + "/bin/llvm-as", "-o", stub_bc, stub_ll],
                  capture_output=True, text=True, timeout=10)
    if r_as.returncode != 0:
        print(f"Stub assembly failed: {r_as.stderr[:200]}")
        return
    # La compilation par lots (plus bas) compile stubs.bc a part et ne relit jamais linked.bc : ce re-lien de
    # ~1 Go n'y sert a rien et a depasse 900 s sous swap (10-21), en vidant linked.bc au passage.
    split = os.environ.get("BOOT_SPLIT_COMPILE", "1") == "1"
    r = subprocess.CompletedProcess([], 0) if split and len(batch_bcs) > 1 else \
        subprocess.run([LLVM17 + "/bin/llvm-link", "-o", linked_bc, linked_bc, stub_bc],
                  capture_output=True, text=True, timeout=900)   # 120 s : trop court a 58 k fonctions (454 Mo)
    if r.returncode != 0:
        print(f"Stub re-link failed: {r.stderr[:200]}")
        print(f"  linked_bc exists: {os.path.exists(linked_bc)}, stub_bc exists: {os.path.exists(stub_bc)}")
        return
    print(f"  Added {len(undefined)} tracing stubs")
    if os.environ.get("BOOT_STOP_AT_BC"):
        # Arret apres linked.bc (outil de developpement). Le clang -O0 (plusieurs Go) et le lien
        # d'ici ne servent pas ; seul main.cpp (ecrit par _finish_build) sert.
        return _finish_build(args, workdir, None, bc_files, time.time())

    # Compile to arm64. At -O1 the codegen of this bitcode is superlinear in the
    # function count: after the general jump-table pass grew it to ~13k fns /95 MB
    # it took ~1.9 h. -O0 compiles the SAME bitcode in ~60 s (2.2 GB, verified
    # byte-identical boot behavior) — the right default for the measure-fix-relift
    # loop. Set BOOT_OPT=-O1 (or -O2) for a performance build of the final .o.
    opt = os.environ.get("BOOT_OPT", "-O0")
    print(f"Compiling to native arm64 ({opt})...")
    lifted_o = os.path.join(workdir, "lifted.o")
    # Compilation par lots (defaut) : un seul clang sur linked.bc (185 Mo, 22 k fonctions) prend
    # plusieurs Go ; sur 16 Go partages, le swap a rempli le disque deux fois (3,4 puis 2,5 Gio libres). Chaque
    # batch_N.bc est deja un module complet (llvm-link l'a produit) ; les stubs en sont un de plus. `ld -r` joint
    # les objets en un lifted.o unique pour la suite (lien, passe 2). Memes symboles que le chemin monolithique.
    if split and len(batch_bcs) > 1:
        from concurrent.futures import ThreadPoolExecutor
        units = batch_bcs + [stub_bc]
        objs = [u[:-3] + ".o" for u in units]
        def _cc(pair):
            return subprocess.run([LLVM17 + "/bin/clang", "-arch", "arm64", "-c", opt, "-x", "ir",
                                   "-o", pair[1], pair[0]], capture_output=True, text=True,
                                  timeout=int(os.environ.get("BOOT_COMPILE_TIMEOUT", "900")))
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            results = list(ex.map(_cc, zip(units, objs)))
        r = next((x for x in results if x.returncode != 0), results[-1])
        if r.returncode == 0:
            r = subprocess.run([LLVM17 + "/bin/clang", "-arch", "arm64", "-r", "-nostdlib", "-o", lifted_o] + objs,
                               capture_output=True, text=True, timeout=600)
        for o in objs:
            if os.path.exists(o):
                os.remove(o)
        print(f"  {len(units)} lots compiles separement, joints par ld -r")
    else:
        r = subprocess.run([LLVM17 + "/bin/clang", "-arch", "arm64", "-c", opt,
                            "-x", "ir", "-o", lifted_o, linked_bc],
                           capture_output=True, text=True,
                           timeout=int(os.environ.get("BOOT_COMPILE_TIMEOUT", "900")))
    if r.returncode != 0:
        errs = [l for l in r.stderr.split("\n") if l and not any(x in l for x in
                ["pentium","cmov","cx8","fxsr","mmx","sse","x87","overriding","loop not","pass-failed"])]
        print(f"Compile failed: {errs[:5]}")
        return
    print(f"  Object: {os.path.getsize(lifted_o)/1024/1024:.1f} MB")
    with open(os.path.join(workdir, "bc_files.txt"), "w") as f:   # pour BOOT_RELINK_ONLY
        f.write("".join(b + "\n" for b in bc_files))
    return _finish_build(args, workdir, lifted_o, bc_files, locals().get("start", time.time()))


def _finish_build(args, workdir, lifted_o, bc_files, start):
    """Runtime, hote et lien final : tout ce qui suit lifted.o. BOOT_RELINK_ONLY=1 y saute directement."""
    # Compile shim runtime
    print("Compiling shim runtime...")
    shim_objs = []
    # ⚠️ THE SHIM IS t6sp-BOUND, AND THAT IS THE REAL WALL — not the lifter.
    # win32_runtime.cpp's register_extra_lifted() includes extra_lifted_vas.inc
    # and unwind_extra_vas.inc, which are lists of t6sp VAs (`BO2_EXTRA_LIFTED(
    # 40135b)` and ~900 more). Against a t6mp lift every one of them is an
    # undefined symbol, because those addresses are functions in a different
    # executable. Same for the interposes' _sub_<va>_impl externs.
    #
    # Shadow both .inc files with EMPTY ones by putting a
    # generated include dir FIRST on the search path. That gives a first t6mp
    # boot host with no extra-lifted registrations and no interposes — which is
    # exactly what a first boot should have: nothing but the lift.
    inc_override = None
    inc_override = os.path.join(workdir, "inc_override")
    os.makedirs(inc_override, exist_ok=True)
    for stub_inc in ("extra_lifted_vas.inc", "unwind_extra_vas.inc"):
        with open(os.path.join(inc_override, stub_inc), "w") as f:
            f.write(f"// GENERATED for BO2_TARGET={TARGET}: intentionally empty.\n"
                    f"// The real {stub_inc} lists t6sp virtual addresses.\n")
    print(f"  BO2_TARGET={TARGET}: -DBO2_NO_EXTRA_LIFTED (t6sp VA lists off), "
      f"-DBO2_TARGET_{TARGET.upper()} (this binary's IAT table)")
    # ...and every interpose still declares `extern "C" void*
    # sub_<t6sp va>_impl(...)` and calls it. Those bodies live in
    # boot_cache/*_impl.o, which are lifts of t6sp functions and meaningless
    # here. Generate a definition for each so the host LINKS; each one aborts
    # loudly if it is ever entered, which it cannot be, because
    # patch_redirect is what routes the guest into an interpose and boot.py
    # does not run it for a non-sp target.
    import re as _re
    syms = set()
    for src_name in ("win32_runtime.cpp", "kernel32_impl.cpp",
                     "user32_impl.cpp", "remaining_dlls_impl.cpp"):
        txt = open(os.path.join(HERE, "..", "runtime", "win32", src_name)).read()
        syms |= set(_re.findall(r"\bsub_([0-9a-f]{6})_impl\b", txt))
    stub_cpp = os.path.join(workdir, "t6sp_interpose_stubs.cpp")
    with open(stub_cpp, "w") as f:
        f.write("// GENERATED by boot.py for BO2_TARGET=%s.\n" % TARGET)
        f.write("// Definitions for the t6sp interpose bodies the shim references.\n")
        f.write("// Unreachable by construction: patch_redirect is not run here.\n")
        f.write("#include <cstdio>\n#include <cstdlib>\n")
        for va in sorted(syms):
            f.write(f'extern "C" void* sub_{va}_impl(void*, unsigned, void*) {{\n'
                    f'    std::fprintf(stderr, "[TARGET] sub_{va}_impl is a t6sp '
                    f'interpose and must not run on {TARGET}\\n");\n'
                    f'    std::abort();\n}}\n')
    print(f"  generated {len(syms)} t6sp interpose stubs for the link")
    ro_stub = os.path.join(workdir, "t6sp_interpose_stubs.o")
    rs = subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1",
                         "-std=c++17", "-o", ro_stub, stub_cpp],
                        capture_output=True, text=True, timeout=600)
    if rs.returncode != 0:
        print("  FATAL: interpose stubs did not compile:")
        print("   ", (rs.stderr or "").strip()[:800])
        return
    shim_objs.append(ro_stub)
    # ⚠️ THE AUDIO OBJECT IS NOT OPTIONAL AT LINK TIME. remaining_dlls_impl.cpp's
    # sound sampler references bo2_audio_counters / _peak / _peaks /
    # _set_format, which live in runtime/audio/coreaudio_out.cpp. Without it the
    # whole boot host fails to link with four undefined symbols — which is what a
    # first t6mp lift hit after every earlier stage succeeded, and it reads like
    # a lift failure when it is a missing translation unit.
    # The full set remaining_dlls_impl.cpp's d3d_vtbl_dispatch and sound sampler
    # reference. boot.py used to link four of these and get away with it because
    # the missing symbols were only reachable from code paths a small lift never
    # instantiated; a t6mp lift instantiates them.
    # Liste du runtime : bo2_replay.cpp (journal de rejeu) et pad_gc.mm
    # (manette) y sont entres plus tard, d'ou « Undefined symbols _bo2_journal_* ».
    # BOOT_STOP_AT_BC : seul main.cpp sert.
    for rf in [] if os.environ.get("BOOT_STOP_AT_BC") else ["win32_runtime.cpp", "kernel32_impl.cpp", "user32_impl.cpp",
               "remaining_dlls_impl.cpp", "bo2_replay.cpp", "heap_code.cpp", "../audio/coreaudio_out.cpp",
               "../gfx/dxbc_to_msl.cpp", "../video/vpx_native.cpp",
               "../gfx/metal_backend.mm", "../input/pad_gc.mm", "x86_guest_vm.cpp", "interp.cpp"] + (
                   # sps : Demonware (P928), VEH (P925), peau (P781).
                   ["exception_adapter.cpp", "dw_local.cpp", "skin_native.cpp"] if TARGET == "sps" else
                   ["exception_adapter.cpp", "dw_local.cpp", "cull_native.cpp", "skin_native.cpp", "match_rules.cpp"]):
        # Flat object name: `../audio/coreaudio_out.cpp` would otherwise put its
        # .o OUTSIDE workdir, in a directory that does not exist.
        ro = os.path.join(workdir,
                          os.path.basename(rf).replace(".cpp", ".o").replace(".mm", ".o"))
        r = subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1",
                            "-std=c++17", "-DADDRESS_SIZE_BITS=32"] +
                           (["-fobjc-arc", "-x", "objective-c++"] if rf.endswith(".mm") else []) +
                           # ⚠️ -I CANNOT SHADOW A QUOTED INCLUDE. `#include
                           # "extra_lifted_vas.inc"` searches the including
                           # file's OWN directory first, so an override dir on
                           # the search path is never consulted — the empty
                           # copies were written and ignored. A macro is the only
                           # way to switch a quoted include off.
                           ["-DBO2_NO_EXTRA_LIFTED"] +
                           # Select this target's IAT table. Without it the shim
                           # patches t6sp's slot VAs and every one of this
                           # binary's 390 slots keeps its on-disk hint/name RVA,
                           # which the guest then calls.
                           [f"-DBO2_TARGET_{TARGET.upper()}"] + [
                            "-I", REM + "/include"] +
                           (["-I", inc_override] if inc_override else []) +
                           ["-I", os.path.join(HERE, "..", "runtime", "win32"),
                            # dxbc_to_msl.cpp includes hlslcc.h from the vendored
                            # translator.
                            "-I", os.path.join(HERE, "..", "third_party", "HLSLcc", "include"),
                            # vpx_native.cpp includes <vpx/vpx_decoder.h> from
                            # the brew libvpx.
                            "-I", "/opt/homebrew/opt/libvpx/include",
                            # heap_code.cpp, exception_adapter.cpp : Unicorn execute du
                            # code x86 d'origine (tas, gestionnaires d'exception).
                            "-I", "/opt/homebrew/opt/unicorn/include",
                            "-o", ro, os.path.join(HERE, "..", "runtime", "win32", rf)],
                           capture_output=True, text=True, timeout=600)
        # 🪤 THIS USED TO BE `if r.returncode == 0: append`, WITH NO ELSE. A shim
        # translation unit that failed to compile was silently dropped from the
        # link, and the run then died at "Link failed: Undefined symbols" naming
        # symbols whose source file had simply never been built. That is how a
        # first t6mp lift — 496 functions lifted, 3.7 MB object, PE sections
        # written — presented as a link error about audio.
        if r.returncode != 0:
            print(f"  FATAL: {rf} did not compile:")
            print("   ", (r.stderr or "").strip()[:1500])
            return
        shim_objs.append(ro)

    # Write PE sections to a binary file for the host to load
    print("Writing PE sections to binary file...")
    sections, pe = read_pe_sections()
    sections_bin = os.path.join(workdir, "pe_sections.bin")
    with open(sections_bin, "wb") as f:
        f.write(struct.pack("<I", len(sections)))
        for va, rva, vsize, raw, name in sections:
            f.write(struct.pack("<II", va, len(raw)))
            f.write(raw)
    total_section_bytes = sum(len(raw) for _, _, _, raw, _ in sections)
    print(f"  {len(sections)} sections, {total_section_bytes/1024/1024:.1f} MB total")

    # Build the boot host — reads sections file, sets up TEB/PEB, runs entry
    print("Building boot host...")
    # Only register functions that were successfully lifted
    # bc_files contains paths like "func_<va>.bc.fixed" — extract the VA
    lifted_vas = set()
    for bc in bc_files:
        m = re.search(r'func_([0-9a-f]+)\.bc', bc)
        if m:
            lifted_vas.add(int(m.group(1), 16))
    reg_lines = "".join(f'    register_jump_target(0x{va:x}, sub_{va:x});\n' for va in lifted_vas)
    decl_lines = "".join(f'extern "C" void* sub_{va:x}(State*, uint32_t, void*);\n' for va in lifted_vas)
    print(f"  Registering {len(lifted_vas)} successfully-lifted functions")
    main_src = os.path.join(workdir, "main.cpp")
    with open(main_src, "w") as f:
        f.write(r'''#include "remill/Arch/X86/Runtime/State.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <vector>
#include <unistd.h>

extern "C" {
    void win32_shim_init();
    void mem_write32_ext(uint32_t, uint32_t);
    uint32_t mem_read32_ext(uint32_t);
    void mem_write8_ext(uint32_t, uint8_t);
    uint8_t mem_read8_ext(uint32_t);
    void set_fsbase(uint32_t);
    void register_jump_target(uint32_t, void*(*)(State*, uint32_t, void*));
    void* sub_96b75c(State*, uint32_t, void*);
    void win32_run_main_thread(State*, uint32_t);
    void __stub_call_fatal(uint32_t va);
''' + decl_lines + r'''}
// Declared inside the extern "C" block above; the definition inherits C linkage,
// so the lifted stubs' `call @__stub_call_fatal` resolve to this symbol.
void __stub_call_fatal(uint32_t va) {
    fprintf(stderr,"[STUB-CALL] sub_%08x CALLED but never lifted -- aborting "
                   "(old behavior returned a silent stale eax and poisoned the run)\n", va);
    abort();
}
static State* g_state=nullptr;
extern "C" uint32_t win32_mem_read32(uint32_t);
extern "C" int g_import_call_count_probe();
extern "C" State* win32_current_guest_state();
extern "C" void win32_dump_crumbs(int);        // last indirect-dispatch trail
// Dump the currently-running guest thread's state + a stack scan of .text-range
// return addresses. Used by the crash handler AND the compute-spin watchdog:
// lifted decrypt/inflate loops are esp-relative (no ebp chain) and make no
// import calls, so a stall watchdog that walks the guest stack is the only way
// to see where a silent compute spin is stuck.
static void dump_guest(const char* why){
    win32_dump_crumbs(128);   // places a pure-lifted-code hang (no imports to log)
    State* cs=win32_current_guest_state(); if(!cs) cs=g_state; if(!cs) return;
    auto&g=cs->gpr;
    fprintf(stderr,"[WATCHDOG %s] eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
        why,g.rax.dword,g.rbx.dword,g.rcx.dword,g.rdx.dword,g.rsi.dword,g.rdi.dword,g.rbp.dword,g.rsp.dword);
    // fastfile streaming ring (0xff34a8 base / 0xff34ac write-off / 0xff34b0 read-ptr):
    // a compute spin here is the async decrypt/inflate churning a garbage block length.
    uint32_t rb=win32_mem_read32(0xff34a8),rw=win32_mem_read32(0xff34ac),rr=win32_mem_read32(0xff34b0);
    fprintf(stderr,"  [ring] base=%08x woff=%08x rdptr=%08x @base:[%08x %08x %08x]\n",
        rb,rw,rr,win32_mem_read32(rb),win32_mem_read32(rb+4),win32_mem_read32(rb+8));
    uint32_t bp=g.rbp.dword;
    for(int i=0;i<16 && bp>=0x1000 && bp<0xFFFFF000;i++){
        fprintf(stderr,"  [chain] bp=%08x ret=%08x\n",bp,win32_mem_read32(bp+4));
        uint32_t nb=win32_mem_read32(bp); if(nb<=bp) break; bp=nb;
    }
    uint32_t sp=g.rsp.dword; int shown=0;
    for(uint32_t a=sp; a<sp+0x400 && shown<24; a+=4){
        uint32_t v=win32_mem_read32(a);
        if(v>=0x401000 && v<0xa314b0){
            fprintf(stderr,"  [stk %08x] ret=%08x args:[%08x %08x %08x %08x]\n",a,v,
                win32_mem_read32(a+4),win32_mem_read32(a+8),win32_mem_read32(a+0xc),win32_mem_read32(a+0x10));
            shown++;
        }
    }
}
static void crash_handler(int sig) {
    fprintf(stderr,"\n=== CRASH (signal %d) ===\n",sig);
    if(g_state) fprintf(stderr,"  EAX=0x%08x ESP=0x%08x\n",g_state->gpr.rax.dword,g_state->gpr.rsp.dword);
    dump_guest("crash");
    _exit(1);
}
#include <pthread.h>
static void* watchdog_fn(void*){
    // A guest loop with no import calls is either a long compute or a hang. Dump
    // every ~10 s of silence, and give up after BO2_STALL_ABORT consecutive dumps
    // (default 4). Without the bail a hung run sat here for eight minutes and was
    // eventually SIGKILLed — and a runaway guest loop can write tens of GB into
    // the sparse guest map on the way out, so ending early is both faster to
    // iterate on and much cheaper. 0 disables the bail.
    const char* e=getenv("BO2_STALL_ABORT");
    const int maxdumps = e ? atoi(e) : 4;
    int last=-1,stall=0,dumps=0;
    for(;;){ struct timespec ts{2,0}; nanosleep(&ts,nullptr);
        int c=g_import_call_count_probe();
        if(c==last){ if(++stall==5){ fprintf(stderr,"[WATCHDOG] no import calls for ~10s (compute spin):\n"); dump_guest("stall"); stall=0;
            if(maxdumps>0 && ++dumps>=maxdumps){
                fprintf(stderr,"[STALL-ABORT] no import calls for ~%d s — the guest is spinning in "
                               "pure lifted code; see the crumb trail above\n", dumps*10);
                _exit(3); } } }
        else { last=c; stall=0; dumps=0; } }
    return nullptr;
}
static void load_pe_sections(const char* path) {
    FILE* f=fopen(path,"rb"); if(!f){fprintf(stderr,"Cannot open %s\n",path);exit(1);}
    uint32_t n; fread(&n,4,1,f);
    for(uint32_t i=0;i<n;i++){uint32_t va,sz; fread(&va,4,1,f);fread(&sz,4,1,f);if(!sz)continue;
    std::vector<uint8_t> d(sz); fread(d.data(),1,sz,f);
    for(uint32_t j=0;j<sz;j++) mem_write8_ext(va+j,d[j]);
    fprintf(stderr,"[BOOT] section 0x%08x: %u bytes\n",va,sz);} fclose(f);
}
int main(int argc,char** argv) {
    signal(SIGSEGV,crash_handler); signal(SIGABRT,crash_handler); signal(SIGBUS,crash_handler);
    { pthread_t wd; pthread_create(&wd,nullptr,watchdog_fn,nullptr); pthread_detach(wd); }
    if(argc<2){fprintf(stderr,"Usage: %s <pe_sections.bin>\n",argv[0]);return 1;}
    fprintf(stderr,"[BOOT] loading PE sections...\n"); load_pe_sections(argv[1]);
    fprintf(stderr,"[BOOT] initializing shim...\n"); win32_shim_init();
    fprintf(stderr,"[BOOT] registering %zu lifted functions...\n",(size_t)''' + str(len(lifted_vas)) + r''');
''' + reg_lines + r'''
    fprintf(stderr,"[BOOT] setting up TEB/PEB...\n");
    uint32_t teb=0x7FFD0000,peb=0x7FFD4000; set_fsbase(teb);
    mem_write32_ext(teb+0x00,0xFFFFFFFF); mem_write32_ext(teb+0x04,0x00100000+0x10000);
    mem_write32_ext(teb+0x08,0x00100000); mem_write32_ext(teb+0x18,teb);
    mem_write32_ext(teb+0x30,peb); mem_write32_ext(teb+0x2C,0x7FFD8000);
    mem_write32_ext(peb+0x08,0); mem_write32_ext(peb+0x0C,0x7FFD5000);
    mem_write32_ext(peb+0x68,0x7FFD6000); mem_write32_ext(peb+0x78,0x00400000);
    const char*cl="{_T["exe"]}"; for(int i=0;cl[i];i++){mem_write8_ext(0x7FFD7000+i*2,cl[i]);mem_write8_ext(0x7FFD7000+i*2+1,0);}
    // PEB->Ldr (PEB_LDR_DATA) : trois listes circulaires et l'entree de l'exe, comme Windows les rend avant le point
    // d'entree. Laissees a zero (Flink = 0), le parcours de InMemoryOrderModuleList par zombies (sub_451ae0,
    // hash ror13 de "KERNEL32.DLL") ne revenait jamais a la tete : boucle sans fin juste apres son CreateThread.
    // Pas d'entree ntdll/kernel32 : le runtime n'a pas d'image pour elles (GetModuleHandle rend 0x400000).
    { const uint32_t ldr=0x7FFD5000, e=0x7FFD5100, nom=0x7FFD5200;
      uint32_t n=0; for(;cl[n];n++){mem_write8_ext(nom+n*2,cl[n]);mem_write8_ext(nom+n*2+1,0);} mem_write32_ext(nom+n*2,0);
      mem_write32_ext(ldr+0x00,0x30); mem_write32_ext(ldr+0x04,1); mem_write32_ext(ldr+0x08,0);
      for(uint32_t k=0;k<2;k++){ const uint32_t tete=ldr+0x0C+k*8, lien=e+k*8;       // InLoadOrder, InMemoryOrder
          mem_write32_ext(tete,lien); mem_write32_ext(tete+4,lien); mem_write32_ext(lien,tete); mem_write32_ext(lien+4,tete); }
      mem_write32_ext(ldr+0x1C,ldr+0x1C); mem_write32_ext(ldr+0x20,ldr+0x1C);       // InInitializationOrder : vide (DLL seules)
      mem_write32_ext(e+0x10,e+0x10); mem_write32_ext(e+0x14,e+0x10);
      mem_write32_ext(e+0x18,0x00400000); mem_write32_ext(e+0x1C,{ENTRY_VA});
      mem_write32_ext(e+0x20,mem_read32_ext(0x00400000+mem_read32_ext(0x0040003C)+0x50));   // SizeOfImage
      for(uint32_t k=0;k<2;k++){ mem_write32_ext(e+0x24+k*8,(n*2)|((n*2+2)<<16)); mem_write32_ext(e+0x28+k*8,nom); }  // Full/BaseDllName
      mem_write32_ext(e+0x34,0); mem_write32_ext(e+0x38,0xFFFF); }
    mem_write32_ext(0x7FFD6000,0x7FFD7000); mem_write32_ext(0x7FFD6000+4,16);
    for(int i=0;i<64;i++) mem_write32_ext(0x7FFD8000+i*4,0);
    // (plus d'ecriture du cookie /GS a 0xea8a70 : t6sp l'a deja dans son fichier, et chez zombies cette
    // adresse est un mot de .data (0 dans le fichier) que le chargeur Windows ne touche pas.)
    fprintf(stderr,"[BOOT] calling entry 0x%x...\n\n", {ENTRY_VA});
    uint32_t sp=0x00100000+0x10000-0x100; mem_write32_ext(sp,0xDEADDEAD);
    State state; memset(&state,0,sizeof(state)); state.gpr.rsp.dword=sp; g_state=&state;
    // FPU defaults (FNINIT/Windows): zeroed cw = "all FP exceptions unmasked"
    // to CRT fnstcw/stmxcsr checks -> spurious STATUS_FLOAT_INEXACT raises.
    state.x87.fxsave.cwd.flat=0x027F; state.x87.fxsave.mxcsr.flat=0x1F80;
    // Lifted code reads the FS base from State.addr.fs_base (not the runtime
    // global set by set_fsbase), so point it at the TEB or every fs:[disp]
    // hits linear `disp` and the TEB is invisible to the guest.
    state.addr.fs_base.dword=teb;
    win32_run_main_thread(&state,{ENTRY_VA});
    fprintf(stderr,"\n[BOOT] entry returned! EAX=0x%08x\n",state.gpr.rax.dword);
    return 0;
}
''')
        # ⚠️ THE GENERATED HOST HARD-CODED t6sp's ENTRY AND COMMAND LINE.
        # boot.py printed "entry: 0xa7fd4c" at the top and then emitted a main.cpp
        # that called 0x96b75c — so the first t6mp host loaded t6mp's PE sections,
        # registered 496 t6mp functions, and jumped to a t6sp address, which
        # reported "NO LIFTED BLOCK at 0x0096b75c" and returned cleanly. It looked
        # like a lift gap and was a template constant. The template is a raw
        # string, so substitute after writing.
        with open(main_src) as _fh: _host = _fh.read()
        _host = (_host.replace("{ENTRY_VA}", hex(ENTRY_VA))
                      .replace('{_T["exe"]}', _T["exe"]))
        with open(main_src, "w") as _fh: _fh.write(_host)

    if os.environ.get("BOOT_STOP_AT_BC"):
        print(f"BOOT_STOP_AT_BC : linked.bc et {main_src} prets, arret avant la compilation de lifted.o")
        return

    # Hors campagne, un `sub_<va>` que le runtime définit ET que le lift définit est une substitution t6sp posée sur une
    # adresse qui, dans cet exe, porte une autre fonction (0x4595f0 = inflate de la campagne, autre code dans le multi).
    # La fonction de CET exe gagne : on rend locale la définition du runtime. SHIM_DEFINED garde les substitutions
    # voulues pour la cible.
    _nm = lambda o: {l.split()[-1] for l in subprocess.run([LLVM17 + "/bin/llvm-nm", "--defined-only", "--extern-only", o],
                     capture_output=True, text=True, timeout=300).stdout.splitlines() if l.strip()}
    _lift = {s for s in _nm(lifted_o) if s.startswith("_sub_")}
    _voulues = {f"_sub_{va:x}" for va in SHIM_DEFINED}
    for _o in shim_objs:
        _loc = sorted((_nm(_o) & _lift) - _voulues)
        if _loc:
            # llvm-objcopy ne sait pas rendre locale une définition Mach-O ; ld -r le fait (et rend locales au
            # passage les fonctions inline cachées de l'objet, que chaque objet qui s'en sert porte déjà).
            subprocess.run(["ld", "-r", "-arch", "arm64"] + [a for s in _loc for a in ("-unexported_symbol", s)]
                           + ["-o", _o + ".loc", _o], check=True)
            os.replace(_o + ".loc", _o)
            print(f"  substitutions t6sp écartées de {os.path.basename(_o)} (le lift gagne) : {', '.join(_loc[:6])}")

    exe = os.path.join(workdir, "boot")
    print("Linking...")
    r = subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1", "-std=c++17",
                        "-DADDRESS_SIZE_BITS=32", "-I", REM + "/include",
                        "-I", os.path.join(HERE, "..", "runtime", "win32"),
                        "-o", exe, main_src, lifted_o, "-Wl,-sectcreate,__TEXT,__info_plist," + os.path.join(HERE, "..", "scripts", "boot_info.plist")] + shim_objs +
                       # AudioToolbox: coreaudio_out.cpp is linked now (its bo2_audio_* symbols are
        # referenced by remaining_dlls_impl.cpp), and it calls AudioQueue*.
        ["-framework", "ApplicationServices", "-framework", "AudioToolbox",
         "-framework", "Cocoa", "-framework", "Metal", "-framework", "QuartzCore",
         "-framework", "GameController", "-framework", "IOKit",
         "-lm", "-lz", "/opt/homebrew/opt/libvpx/lib/libvpx.a",
         "/opt/homebrew/opt/unicorn/lib/libunicorn.a",
         # HLSLcc provides TranslateHLSLFromMem, which dxbc_to_msl.cpp calls.
         os.path.join(HERE, "..", "boot_cache", "hlslcc_build", "libhlslcc.dylib")],
                       capture_output=True, text=True, timeout=900)
    # ⚠️ SECOND PASS FOR A NON-SP TARGET, DRIVEN BY THE LINKER ITSELF.
    # Scraping the sources for `sub_<va>_impl` is not complete: some are produced
    # by a MACRO (BO2_ESP_AUDIT(8d2c70) expands to sub_8d2c70_impl, so the
    # literal symbol never appears in the text), and some interposes call the
    # bare `sub_<va>` with no _impl at all. Rather than chase every spelling,
    # ask the linker what is missing and define exactly that.
    if r.returncode != 0:
        import re as _re2
        missing = sorted(set(_re2.findall(r'"_?(sub_[0-9a-f]{6}(?:_impl\d*)?)"', r.stderr)))
        # ⚠️ NEVER DEFINE A SYMBOL THE LIFT ALREADY PROVIDES. On multi this pass
        # emitted `sub_435d30`, which is both a t6sp interpose name AND a real
        # function in another build's lift set, so the second link died with
        # `duplicate symbol '_sub_435d30'`. A t6sp-only VA is only t6sp-only for
        # the binary it came from; on another target the same address is
        # ordinary code. Ask the object file instead of assuming.
        if missing:
            try:
                nm = subprocess.run([LLVM17 + "/bin/llvm-nm", "--defined-only",
                                     lifted_o], capture_output=True, text=True,
                                    timeout=300)
                have = {l.split()[-1].lstrip("_") for l in nm.stdout.splitlines()
                        if l.strip()}
                clash = [s for s in missing if s in have]
                if clash:
                    print(f"  link pass 2: {len(clash)} symbol(s) are defined by the "
                          f"lift itself, leaving them to it: {', '.join(clash[:6])}")
                    missing = [s for s in missing if s not in have]
            except Exception as _e:
                print(f"  link pass 2: could not read {lifted_o} symbols ({_e}); "
                      f"proceeding without the clash filter")
        if missing:
            print(f"  link pass 2: defining {len(missing)} more t6sp symbols the "
                  f"linker named")
            extra_cpp = os.path.join(workdir, "t6sp_extra_stubs.cpp")
            with open(extra_cpp, "w") as f:
                f.write("// GENERATED by boot.py: symbols the first link pass reported\n")
                f.write("#include <cstdio>\n#include <cstdlib>\n")
                for sym in missing:
                    f.write(f'extern "C" void* {sym}(void*, unsigned, void*) {{\n'
                            f'    std::fprintf(stderr, "[TARGET] {sym} is t6sp-only\\n");\n'
                            f'    std::abort();\n}}\n')
            eo = os.path.join(workdir, "t6sp_extra_stubs.o")
            re2 = subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c",
                                  "-O1", "-std=c++17", "-o", eo, extra_cpp],
                                 capture_output=True, text=True, timeout=600)
            if re2.returncode == 0:
                r = subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1",
                                    "-std=c++17", "-DADDRESS_SIZE_BITS=32",
                                    "-I", REM + "/include",
                                    "-I", os.path.join(HERE, "..", "runtime", "win32"),
                                    "-o", exe, main_src, lifted_o, "-Wl,-sectcreate,__TEXT,__info_plist," + os.path.join(HERE, "..", "scripts", "boot_info.plist")] + shim_objs + [eo] +
                                   ["-framework", "ApplicationServices",
                                    "-framework", "AudioToolbox", "-framework", "Cocoa", "-framework", "GameController",
                                    "-framework", "Metal", "-framework", "QuartzCore", "-framework", "IOKit",
                                    "-lm", "-lz",
                                    "/opt/homebrew/opt/libvpx/lib/libvpx.a",
                                    "/opt/homebrew/opt/unicorn/lib/libunicorn.a",
                                    os.path.join(HERE, "..", "boot_cache",
                                                 "hlslcc_build", "libhlslcc.dylib")],
                                   capture_output=True, text=True, timeout=900)
            else:
                print("  FATAL: extra stubs did not compile:", (re2.stderr or "")[:400])

    if r.returncode != 0:
        # 🪤 THE FILTER USED TO SHOW THE FIRST 8 NON-"duplicate" LINES, and once
        # libvpx.a joined the link those were EIGHT `ld: warning: object file …
        # was built for newer macOS version` lines. The report read "Link
        # failed:" followed by nothing but warnings, with the actual undefined
        # symbol never printed. Rank real errors first.
        # ⚠️ DO NOT FILTER "duplicate". `duplicate symbol '_sub_x' in: a.o b.o`
        # is a HARD error, and dropping it printed the two indented .o paths with
        # the line that named the symbol removed — a "Link failed:" report whose
        # entire content was two file names. Only `ld: warning:` is noise here.
        lines = [l for l in r.stderr.split("\n") if l.strip()]
        hard = [l for l in lines if "warning" not in l.lower()]
        errs = (hard or lines)[:14]
        print("Link failed:")
        for l in errs:
            print("   ", l[:200])
        return

    print(f"  Binary: {os.path.getsize(exe)/1024/1024:.1f} MB")

    # Post-link: redirect the guest's MSVC longjmp (0x965790) to the shim's
    # bo2_longjmp. It is reached by a DIRECT `bl _sub_965790` to a strong lifted
    # symbol, so it cannot be overridden at link time — the entry is patched in
    # the binary instead. Without this the lifted longjmp falls through to int3
    # and every Lua GC-step yield kills the thread.
    # ⚠️ THE LONGJMP VA IS PER-TARGET, AND GETTING IT WRONG IS NOT SUBTLE. Every
    # t6mp run printed "patch_redirect FAILED — the binary will crash on the
    # first guest longjmp" because 0x965790 is t6sp's address, and then crashed
    # exactly as advertised: the lifted longjmp RETURNED instead of unwinding and
    # fell through into int3 padding, which the crumbs show as two dispatches one
    # byte apart (0x487af6, then 0x487af7). Three harvest rounds chased that as
    # if it were a missing block; it was the warning the build had already
    # printed, five lines above, in every single run.
    LJ = _T.get("longjmp")
    if LJ is None:
        print(f"Patching guest longjmp... SKIPPED: no longjmp VA known for BO2_TARGET={TARGET}.")
        r = None
    else:
        print(f"Patching guest longjmp ({LJ:#x})...")
        r = subprocess.run([sys.executable, os.path.join(HERE, "patch_redirect.py"), exe,
                        f"_sub_{LJ:x}=_bo2_longjmp"] + [f"_sub_{v:x}={d}" for v, d in _T.get("redirects", {}).items()],
                       capture_output=True, text=True, timeout=600)
    if r is not None:
        print("  " + (r.stdout.strip() or r.stderr.strip())[:300])
    if r is None or r.returncode != 0:
        print("  patch_redirect FAILED — the binary will crash on the first guest longjmp")
    else:
        # MANDATORY: patching invalidates the linker's ad-hoc code signature, and
        # the arm64 kernel then kills the process on a page-fault of the modified
        # page — a silent early exit that looks exactly like a runtime hang.
        rs = subprocess.run(["codesign", "-f", "-s", "-", exe],
                            capture_output=True, text=True, timeout=600)
        print("  re-signed" if rs.returncode == 0
              else f"  codesign FAILED: {rs.stderr.strip()[:200]} — process will be killed on run")

    if args.build_only:
        if r is None or r.returncode != 0 or rs.returncode != 0:
            raise SystemExit("build-only failed: redirect or signature")
        print(f"BUILD-ONLY READY: {exe} {sections_bin}", flush=True)
        return

    # RUN IT
    print("\n" + "="*60)
    print("RUNNING BOOT ATTEMPT")
    print("="*60 + "\n")

    # errors="replace" : un seul octet non UTF-8 dans la trace (0xe8, un « è » Latin-1
    # du jeu, tour 2/3) levait UnicodeDecodeError ici, apres le run : la
    # trace de 12 Mo etait perdue et la boucle d'iteration concluait « converged ». Meme sort au
    # delai : TimeoutExpired n'ecrivait rien, alors qu'un invite qui a ouvert sa
    # fenetre tourne jusqu'au delai. On garde la trace partielle.
    try:
        r = subprocess.run([exe, sections_bin], capture_output=True, text=True,
                           errors="replace", timeout=600)
    except subprocess.TimeoutExpired as _to:
        _dec = lambda b: b.decode("utf-8", "replace") if isinstance(b, bytes) else (b or "")
        r = subprocess.CompletedProcess(_to.cmd, -9, _dec(_to.stdout),
                                        _dec(_to.stderr) + "\n=== TIMEOUT (600 s, invite tue) ===\n")
    print("--- STDOUT ---")
    print(r.stdout[:2000] if r.stdout else "(empty)")
    print("\n--- STDERR (trace) ---")
    # ⚠️ ALWAYS KEEP THE WHOLE TRACE. This used to print the first 100 and last
    # 20 lines and DISCARD the middle, which is exactly where the answer lives:
    # the multi crash dump printed `[WATCHDOG crash]` with its `=== CRASH (signal
    # N) ===` header — written immediately before it by the same handler — inside
    # the dropped section, so the log could not even say whether the run had
    # faulted or stalled. Console output stays summarised; the file is complete.
    trace_lines = r.stderr.split("\n") if r.stderr else []
    full_trace = os.path.join(HERE, "..", "boot_cache", f"trace_{TARGET}.log")
    try:
        os.makedirs(os.path.dirname(full_trace), exist_ok=True)
        with open(full_trace, "w") as _tf:
            _tf.write(r.stderr or "")
        print(f"[full trace: {os.path.normpath(full_trace)} "
              f"({len(trace_lines)} lines)]")
    except Exception as _e:
        print(f"[could not save the full trace: {_e}]")
    for line in trace_lines[:100]:
        print(line)
    if len(trace_lines) > 100:
        print(f"\n... ({len(trace_lines) - 100} more lines — see the full trace above)")
        # Surface the first hard failure from the DROPPED middle, so a summarised
        # console never hides the one line that names what went wrong.
        _key = [l for l in trace_lines[100:-20]
                if any(k in l for k in ("=== CRASH", "signal", "RUNAWAY",
                                        "unknown target", "[MISSING]",
                                        "NOT IMPLEMENTED", "abort"))]
        if _key:
            print(f"--- {len(_key)} notable line(s) from the middle ---")
            for line in _key[:12]:
                print(line)
        print("\n--- LAST 20 LINES ---")
        for line in trace_lines[-20:]:
            print(line)

    print(f"\n--- EXIT CODE: {r.returncode} ---")
    total_time = time.time() - start
    print(f"Total time: {total_time:.1f}s")


if __name__ == "__main__":
    main()
