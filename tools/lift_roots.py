#!/usr/bin/env python3
"""Root-seeded lift-set expansion.

The call-tree walk that seeds the lifter (boot.py's first-N calltree rows) is a
*static* traversal — it cannot see through indirect dispatch. The CRT runs its C
and C++ initializers by calling through the __xi_a..__xi_z / __xc_a..__xc_z
pointer tables; those targets (and everything they call) are invisible to a
static BFS, so they never get lifted and fault at runtime.

This module fixes that class generally: given a set of ROOT addresses, it
computes their closure over *direct* call / tail-jmp edges — the same set a BFS
would reach if it could see through the indirect dispatch. Roots today come from
the initializer tables; the same mechanism accepts more pointer tables (vtables,
callback registries) or a file of VAs harvested from runtime
__remill_missing_block hits, which is how we'll keep widening coverage as
execution goes deeper.

A root or callee may be an address Ghidra never recognized as a function (the
compiler-generated initializer thunks are). For those we synthesize the extent
by disassembling from the address until a terminating ret/jmp, bounded by the
next known function start.
"""
from __future__ import annotations
import csv, os, bisect, re
from typing import Iterable
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

# CRT initializer pointer tables (guest VAs), read out of each image's _cinit.
#   __xc_a..__xc_z : C++ dynamic initializers (constructors)
#   __xi_a..__xi_z : C initializers
#
# ⚠️ PER-BINARY. These were t6sp's values applied to all three targets, which
# meant mp and zm seeded their initializer closure from whatever bytes happen to
# live at t6sp's addresses — measured content there: sp 15/16 valid pointers,
# mp 0/16, zm 6/16. On mp those were raw instruction bytes being lifted as
# functions. Same bug class as the IAT (see tools/gen_iat_header.py).
#
# Found in each binary's _cinit, which calls _initterm_e(__xi_a, __xi_z) and
# then inlines the __xc walk as `mov eax,__xc_a; mov esi,__xc_z`:
#     t6sp 0x966d3e/0x966d5a   t6mp 0xa7b5a9/0xa7b5ca   t6zm 0xa74f69/0xa74f8a
# __xi_a is 4 bytes past __xc_z in all three.
#
# ⚠️⚠️ AND ON mp/zm THE TABLES ARE ALL-ZERO ON DISK. t6sp ships 271 constructors
# and 7 C initializers; t6mp and t6zm ship 318 and 8 slots that are every one
# null. That is why __onexitinit (mp 0xa75330) is never called, why nothing in
# the image points to it, and why the CRT dies in __onexit on an unallocated
# atexit table. Seeding from them is correct but currently yields nothing —
# something must populate them at runtime and we do not yet know what.
#
# POPULATED (317/318 and 315/317 constructors, 7/8 C initializers) — these are
# the only mp/zm targets whose CRT can actually initialise. Read from each
# build's own _cinit; two builds of the same exe can differ by 4 bytes, which is why they are measured per binary and never assumed.
_TABLES: dict[str, tuple[tuple[int, int], ...]] = {
    "sps": ((0x9ef574, 0x9ef9c4), (0x9ef9c8, 0x9ef9e8)),  # Steam _cinit 0x92017a-0x9201a0, globaux appariés
    "mps": ((0xb095e0, 0xb09ad4), (0xb09ad8, 0xb09af8)),  # Steam _cinit 0xa2b55d (0xa7b58d dans une ancienne t6mp)
    "zms": ((0xb075e0, 0xb07ad4), (0xb07ad8, 0xb07af8)),  # Steam _cinit 0xa2868d (0xa747ed dans une ancienne t6zm)
}
DEFAULT_TABLES: tuple[tuple[int, int], ...] = _TABLES[os.environ.get("BO2_TARGET", "sps")]
SYNTH_CAP = 0x1000                       # max bytes to synthesize for one unlisted function
_TERM = {"ret", "retn", "iret", "iretd", "int3"}
_md = Cs(CS_ARCH_X86, CS_MODE_32)
_HEX_ONLY = re.compile(r"^0x[0-9a-f]+$")


def _load_functions(ghidra_dir: str) -> dict[int, tuple[int, str, str]]:
    """va -> (size, bytes_hex, name) over every Ghidra-known function."""
    csv.field_size_limit(1 << 30)
    idx: dict[int, tuple[int, str, str]] = {}
    # La colonne `va` n'a pas la meme base selon le fichier : DECIMALE dans all_functions.csv
    # ("4199456"), hexadecimale sans prefixe dans calltree.csv ("00a78fbc"). Lue en base 16 partout,
    # all_functions.csv donnait des cles hors image : tout le catalogue Ghidra etait perdu, et chaque
    # fonction atteinte par les racines etait synthetisee et tronquee a SYNTH_CAP (Zombies : 10 902 sur
    # 22 845 ; 0xa5aaa0, SHA-256 de 7 529 octets, liftee sur 4 092 -> __remill_error a +4155).
    for fname, base in (("all_functions.csv", 10), ("calltree.csv", 16)):
        path = os.path.join(ghidra_dir, fname)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            for row in csv.DictReader(f):
                try:
                    va = int(row["va"], base); sz = int(row["size"])
                except (KeyError, ValueError):
                    continue
                idx.setdefault(va, (sz, row["bytes_hex"], row["name"]))
    return idx


def _exec_ranges(pe) -> list[tuple[int, int]]:
    ranges = []
    for s in pe.sections:
        ch = getattr(s, "characteristics", 0)
        if ch & 0x20000000 or s.name.strip("\x00") == ".text":   # IMAGE_SCN_MEM_EXECUTE
            a = pe.image_base + s.virtual_address
            ranges.append((a, a + s.virtual_size))
    return ranges


def read_pointer_table(pe, start: int, end: int) -> list[int]:
    """Non-null 4-byte pointers in a guest [start, end) pointer table."""
    out = []
    for v in range(start, end, 4):
        try:
            off = pe.rva_to_offset(v - pe.image_base)
            p = int.from_bytes(pe._raw[off:off + 4], "little")
        except Exception:
            continue
        if p:
            out.append(p)
    return out


# Opt-in (LIFT_FOLLOW_JCC=1, armed by scripts/mp_iterate.sh only): a jcc target
# outside the chunk's extent is a root, like a jmp target. Cold blocks placed past
# the Ghidra extent, and backward branches out of a separately lifted jump-table
# case, were otherwise MISSING at runtime — one abort per round (Zombies, P703).
_FOLLOW_JCC = os.environ.get("LIFT_FOLLOW_JCC") == "1"


class _Closure:
    def __init__(self, pe, idx: dict[int, tuple[int, str, str]]):
        self.pe = pe
        self.idx = idx
        self.starts = sorted(idx.keys())
        self.exec_ranges = _exec_ranges(pe)

    def _in_code(self, va: int) -> bool:
        return any(a <= va < b for a, b in self.exec_ranges)

    def _next_start(self, va: int) -> int:
        i = bisect.bisect_right(self.starts, va)
        return self.starts[i] if i < len(self.starts) else va + SYNTH_CAP

    def _img(self, va: int, n: int) -> bytes:
        try:
            off = self.pe.rva_to_offset(va - self.pe.image_base)
            return self.pe._raw[off:off + n]
        except Exception:
            return b""

    def instruction_boundary(self, va: int, back: int = 0xa00):
        """True / False / None(undetermined): is `va` the start of an instruction?

        Roots harvested from a *run* are not trustworthy the way a pointer table
        is. When the guest computes a dispatch target from corrupt state it can
        land in the middle of an instruction, and `_decode` will happily
        synthesize a "function" there by disassembling the tail bytes of a real
        instruction — lifting nonsense and registering it under a VA that is not
        a function. Measured on the first harvest round: 11 of 14 addresses for
        t6mp and 11 of 13 for t6zm were mid-instruction. `0x93ff2a` fell inside
        `lea edx,[esp+0x68]` at 0x93ff29.

        Anchor: MSVC pads between functions with int3, so the last
        `0xCC 0xCC -> non-0xCC` transition below `va` is a function start with
        high probability. Linear-sweep from there and ask whether `va` is one of
        the instruction addresses produced.

        Returns None when no anchor is found, and callers KEEP those — an
        unanchored address is unproven, not disproven.

        A catalogued function is accepted without sweeping. That is not a
        shortcut, it is what makes the filter exact: measured over 600 sampled
        Ghidra function starts, every single address the sweep rejected was one
        Ghidra already knew (16/16), while *none* of the 22 mid-instruction
        harvest addresses were catalogued (0/22). Deferring to the catalogue
        therefore removes the sweep's whole 2-3% false-drop rate and still
        rejects every bogus root.
        """
        if va in self.idx:
            return True
        if not self._in_code(va):
            return False
        blob = self._img(va - back, back)
        if len(blob) < 4:
            return None
        anchor = None
        for k in range(len(blob) - 1, 2, -1):
            if blob[k] != 0xCC and blob[k - 1] == 0xCC and blob[k - 2] == 0xCC:
                anchor = va - back + k
                break
        if anchor is None or anchor >= va:
            return None
        code = self._img(anchor, va - anchor + 32)
        return any(i.address == va for i in _md.disasm(code, anchor))

    def _decode(self, va: int, sans_borne: bool = False) -> tuple[int, list[int], list[int]]:
        """(size, call_targets, jmp_targets). Uses recorded bytes for a known
        function; for an unlisted one, detects the extent by linear sweep —
        ending only at a ret / unconditional jmp once we are past every pending
        forward branch target (so intra-function forward jumps don't truncate
        the function, and calls after them are still discovered)."""
        if va in self.idx:
            _, bh, _ = self.idx[va]
            code = bytes.fromhex(bh); region_end = va + len(code); bounded = False
        else:
            region_end = va + SYNTH_CAP if sans_borne else min(self._next_start(va), va + SYNTH_CAP)
            code = self._img(va, region_end - va); bounded = True
        calls, jmps, size = [], [], 0
        max_fwd = va                          # furthest forward branch target seen
        for insn in _md.disasm(code, va):
            size = insn.address + insn.size - va
            insn_end = insn.address + insn.size
            m, op = insn.mnemonic, insn.op_str
            # Only a LONE hex literal is a branch target. A linear sweep over an
            # unlisted function eventually decodes padding/data, and operands like
            # "0x5b, al" (out imm8, al) start with "0x" but are not addresses —
            # int() then raised ValueError and killed the whole run.
            tgt = int(op, 16) if _HEX_ONLY.match(op) else None
            if m == "call":
                if tgt is not None:
                    calls.append(tgt)
            elif m == "jmp":
                if tgt is not None:
                    jmps.append(tgt)
                    if bounded and va <= tgt < region_end and tgt > max_fwd:
                        max_fwd = tgt         # intra-function forward jump
                if bounded and insn_end > max_fwd:
                    break                     # unconditional jmp, no pending forward branch = end
            elif m and m[0] == "j":           # jcc
                if tgt is not None and _FOLLOW_JCC:
                    jmps.append(tgt)
                if bounded and tgt is not None and va <= tgt < region_end and tgt > max_fwd:
                    max_fwd = tgt
            elif m in _TERM:
                if bounded and insn_end > max_fwd:
                    break                     # ret, no pending forward branch = end
        if bounded and size == 0 and not sans_borne:
            # Un faux départ du catalogue au milieu de la PREMIÈRE instruction (0xa2e286 coupé à 0xa2e28a) : rien ne se
            # décode, la racine était jetée sans un mot. Balayage sans borne, comme tools/lift_extra.py.
            return self._decode(va, sans_borne=True)
        return size, calls, jmps

    def expand(self, roots: Iterable[int]) -> dict[int, int]:
        """va -> synthesized size (0 if the function is Ghidra-known)."""
        seen: dict[int, int] = {}
        stack = []
        for r in roots:
            if self._in_code(r) and r not in seen:
                seen[r] = 0; stack.append(r)
        while stack:
            va = stack.pop()
            size, calls, jmps = self._decode(va)
            if va not in self.idx:
                seen[va] = size                    # remember synthesized extent
            fn_end = va + size
            for t in calls:                        # call target = function entry
                if t not in seen and self._in_code(t):
                    seen[t] = 0; stack.append(t)
            for t in jmps:
                if t in seen or not self._in_code(t):
                    continue
                # follow a jmp as a function if it's a known function OR it lands
                # outside this function's extent (a tail call, e.g. the C++ ctor
                # thunk `mov ecx,this; jmp ctor`). Intra-function jumps stay within
                # [va, fn_end) and are skipped.
                if t in self.idx or not (va <= t < fn_end):
                    seen[t] = 0; stack.append(t)
        return seen


def _dword_hits(pe, va: int) -> list[int]:
    """Every file offset where the dword `va` sits, at any alignment, in order: what `raw.find` in a loop returns.
    One scan of the 12 MB image per address was 95 % of the lift's first 10 minutes (12 k addresses, profiled on
    t6mp); the dwords that point into code are indexed once instead (~150 k offsets, about 1 s)."""
    raw = pe._raw
    r = _exec_ranges(pe)
    lo, hi = min(a for a, _ in r), max(b for _, b in r)
    if not lo <= va < hi:
        pat = va.to_bytes(4, "little")
        out, j = [], raw.find(pat)
        while j >= 0:
            out.append(j); j = raw.find(pat, j + 1)
        return out
    if getattr(pe, "_dword_index", None) is None:
        idx: dict[int, list[int]] = {}
        for a in range(4):   # memoryview "I" is native order: little-endian on arm64, as in the PE
            for i, v in enumerate(memoryview(raw[a:a + (len(raw) - a) // 4 * 4]).cast("I")):
                if lo <= v < hi:
                    idx.setdefault(v, []).append(a + 4 * i)
        for l in idx.values():
            l.sort()
        pe._dword_index = idx
    return pe._dword_index.get(va, [])


def _dispatched_table(pe, va: int, span: int = 0x1000) -> list[int]:
    """Siblings of `va` in a table that an indirect `jmp [reg*4+table]` reads."""
    raw = pe._raw
    out: set[int] = set()
    for j in _dword_hits(pe, va):
        near = lambda k: abs(int.from_bytes(raw[k:k + 4], "little") - va) < span
        st = j
        while st >= 4 and near(st - 4):
            st -= 4
        en = j
        while en + 4 <= len(raw) and near(en):
            en += 4
        sec = next((x for x in pe.sections
                    if x.pointer_to_raw_data <= st
                    < x.pointer_to_raw_data + x.size_of_raw_data), None)
        if sec is None:
            continue
        table_va = (pe.image_base + sec.virtual_address
                    + st - sec.pointer_to_raw_data)
        ops = table_va.to_bytes(4, "little")
        if not any(b"\xff\x24" + bytes([m]) + ops in raw
                   for m in (0x85, 0x8d, 0x95, 0x9d, 0xad, 0xb5, 0xbd)):
            continue
        for k in range(st, en, 4):
            out.add(int.from_bytes(raw[k:k + 4], "little"))
    return sorted(out - {va})


def jump_table_siblings(pe, clo, va: int) -> list[int]:
    """Every landing site sharing a jump table with `va`.

    Indirect landing sites arrive from the runtime ONE PER ROUND, because each
    only becomes reachable once the previous block exists — an older t6mp rounds 3 and 4
    each surfaced exactly one address inside the same function. But they are not
    scattered: MSVC emits them as a contiguous table of pointers back into the
    function, so the whole set is sitting next to the one the guest asked for.

    Measured on an older t6mp `0x9b099c`: the table at `0x9b0a3c..0x9b0a5c` holds all
    EIGHT sites (0x9b0951 0x9b096a 0x9b0983 0x9b099c 0x9b09eb 0x9b0a04 0x9b0a1d
    0x9b0a33), collapsing eight rounds into one. memcpy's `0xa72d70` is the same
    shape with a one-entry table.

    Bounded to the enclosing catalogued function, so a coincidental dword
    elsewhere cannot drag in unrelated addresses.
    """
    i = bisect.bisect_right(clo.starts, va) - 1
    if i < 0:
        return []
    lo = clo.starts[i]
    size = clo.idx.get(lo, (0, "", ""))[0]
    hi = lo + size
    raw = pe._raw
    if not (lo <= va < hi):
        # Ghidra can cut a function short of its own switch cases. Measured on
        # an older t6zm FUN_0057b5f0 (a Duff's-device CRC32): catalogued size 305, ends at
        # 0x57b721, but its `jmp [ebp*4+0x57b80c]` lands on 0x57b721..0x57b803,
        # so the sibling pass was skipped and the 8 cases came one per round.
        # Then bound the table by the dispatch itself: a run of dwords within
        # 4 KiB of `va`, whose start is the operand of a `jmp [reg*4+table]`.
        return _dispatched_table(pe, va)
    out: set[int] = set()
    for j in _dword_hits(pe, va):
        # walk the contiguous run of dwords that all point into THIS function
        st = j
        while st >= 4:
            p = int.from_bytes(raw[st - 4:st], "little")
            if not (lo <= p < hi):
                break
            st -= 4
        en = j
        while en + 4 <= len(raw):
            p = int.from_bytes(raw[en:en + 4], "little")
            if not (lo <= p < hi):
                break
            en += 4
        for k in range(st, en, 4):
            out.add(int.from_bytes(raw[k:k + 4], "little"))
    return sorted(out - {va})


# Opt-in (LIFT_CALL_TABLES=1, armed by scripts/mp_iterate.sh only).
_CALL_TABLES = os.environ.get("LIFT_CALL_TABLES") == "1"
_CALL_TABLE_REACH = 64      # entries walked on each side of the hit
_CALL_TABLE_MAX_TABLES = 16  # a target in more tables than this is a shared stub


def call_table_siblings(pe, clo, harvested: Iterable[int]) -> list[int]:
    """Every entry of the data-side function-pointer tables holding a harvested
    call target.

    An indirect call reaches the runtime ONE TARGET PER ROUND, like a jump-table
    landing site — but its siblings sit in a table outside .text rather than in
    the function. Measured on an older t6zm `call [eax*4+0xd40050]` (0x7fc775, one handler
    per XAsset type): 0x7fe610 came from a round, while 0x7fe630, 0x7fe640,
    0x7fe660 and 0x9b0a60 sat unlifted in the same table.

    A table is a run of consecutive dwords that all point into executable
    sections — no zero, since a null entry is as often the end of one table as a
    hole in it. A target found in more than _CALL_TABLE_MAX_TABLES runs is a
    shared stub (`xor eax,eax; ret` in every vtable): following it would drag in
    every class, so it is skipped. Entries that land strictly inside a catalogued
    function are not function pointers and are dropped.
    """
    import numpy as np
    starts, idx = clo.starts, clo.idx

    def inside(va: int) -> bool:
        i = bisect.bisect_right(starts, va) - 1
        return i >= 0 and starts[i] < va < starts[i] + idx[starts[i]][0]

    cand = np.array(sorted({v for v in harvested if clo._in_code(v) and not inside(v)}),
                    dtype=np.uint32)
    if not len(cand):
        return []
    runs: dict[int, dict[int, frozenset]] = {}
    for s in pe.sections:
        if getattr(s, "characteristics", 0) & 0x20000000 or s.name.strip("\x00") == ".text":
            continue
        n = min(s.size_of_raw_data, s.virtual_size) & ~3
        arr = np.frombuffer(pe._raw[s.pointer_to_raw_data:s.pointer_to_raw_data + n], dtype="<u4")
        code = np.zeros(len(arr), bool)
        for a, b in clo.exec_ranges:
            code |= (arr >= a) & (arr < b)
        for h in np.nonzero(np.isin(arr, cand))[0]:
            st = h
            while st > 0 and h - st < _CALL_TABLE_REACH and code[st - 1]:
                st -= 1
            en = h + 1
            while en < len(arr) and en - h < _CALL_TABLE_REACH and code[en]:
                en += 1
            ent = frozenset(int(x) for x in arr[st:en])
            if len(ent) >= 2:
                runs.setdefault(int(arr[h]), {})[(s.virtual_address, int(st))] = ent
    out: set[int] = set()
    for va, tabs in runs.items():
        if len(tabs) > _CALL_TABLE_MAX_TABLES:
            continue
        for ent in tabs.values():
            out |= {p for p in ent if not inside(p)}
    return sorted(out)


def extra_functions_from_roots(pe, ghidra_dir: str,
                               tables: Iterable[tuple[int, int]] = DEFAULT_TABLES,
                               extra_roots: Iterable[int] = (),
                               roots_file: str | None = None):
    """Closure of the initializer tables (+ extra_roots / roots_file) over
    direct-call edges, as a list of (va, rva, size, bytes_hex, name) ready to
    feed the lifter. Synthesizes extents for functions Ghidra didn't identify.
    """
    idx = _load_functions(ghidra_dir)
    clo = _Closure(pe, idx)
    roots: list[int] = list(extra_roots)
    for a, z in tables:
        roots += read_pointer_table(pe, a, z)
    if roots_file and os.path.exists(roots_file):
        # Runtime-harvested VAs, unlike the pointer tables above, are only as
        # good as the state the guest computed them from. Validate each one is
        # real code before it becomes a lift root (see instruction_boundary).
        harvested = []
        with open(roots_file) as f:
            for line in f:
                line = line.strip().split("#", 1)[0].strip()
                if line:
                    harvested.append(int(line, 16))
        # ⚠️ REJECT ON THE EXACT TEST, NOT THE HEURISTIC ONE.
        #
        # The poison this filter exists for is an UNRESOLVED IMPORT: when a slot
        # is unpatched the guest calls the slot's on-disk value, which for an
        # unbound import is the hint/name RVA. So the deductive test is "does
        # this address equal an IAT slot's file value". Measured:
        #     stock t6mp poison   12/13 are IAT values (the 13th is real code)
        #     stock t6zm poison   13/13
        #     an older t6mp block targets    0/2
        #
        # The instruction-boundary sweep is NOT a substitute and must not reject
        # on its own. It wrongly rejected an older t6mp's 0xa72d70 and 0xa73a7c, which are
        # legitimate mid-function jump targets inside memcpy — the sweep desyncs
        # on memcpy's embedded jump table — and those still need lifting as
        # BLOCKS. "Not a function start" and "not code" are different claims, and
        # conflating them starved the lift of exactly the return/landing sites
        # t6sp needed 362 of. It is kept as an advisory note only.
        iat_vals = set()
        try:
            for a, z in ((getattr(pe, "iat_start", 0), 0),):
                pass
            for entry in getattr(pe, "imports", []):
                for th in entry.thunks:
                    off = pe.rva_to_offset(th.iat_rva)
                    iat_vals.add(int.from_bytes(pe._raw[off:off + 4], "little"))
        except Exception:
            iat_vals = set()

        kept, bogus, unproven = [], [], []
        vus, recoltes = set(), set(harvested)   # `in` on the 52 k-entry lists was ~6 min of the lift's start (sps)
        for va in harvested:
            if va in iat_vals:
                bogus.append(va)
                continue
            kept.append(va); vus.add(va)
            if clo.instruction_boundary(va) is not True:
                unproven.append(va)
            # Always pull in the rest of this address's jump table rather than
            # waiting for the runtime to ask for them one round each. Bounded to
            # the enclosing catalogued function, so this cannot run away — and it
            # must NOT be gated on instruction_boundary(), which returns True for
            # any catalogued address and so skipped exactly the sites that need
            # it (0x9b099c is catalogued AND a landing site).
            # Hors du code, pas de table : l'ancienne liste de racines finissait par 6 VA étendues en signe
            # (0xffffffff73456401…) que to_bytes(4) refusait — lift neuf mort à 12 min (P938).
            sibs = [x for x in jump_table_siblings(pe, clo, va)
                    if x not in recoltes and x not in vus] if clo._in_code(va) else []
            if sibs:
                kept.extend(sibs); vus.update(sibs)
                print(f"  + {len(sibs)} sibling landing site(s) from "
                      f"0x{va:x}'s jump table: "
                      f"{', '.join(hex(x) for x in sibs[:8])}")
        if _CALL_TABLES:
            known = set(kept)
            tabs = [x for x in call_table_siblings(pe, clo, kept) if x not in known]
            if tabs:
                kept.extend(tabs)
                print(f"  + {len(tabs)} entr(ies) from the function-pointer tables "
                      f"holding harvested call targets: "
                      f"{', '.join(hex(x) for x in tabs[:8])}")
        roots += kept
        if harvested:
            _direct = len([v for v in kept if v in recoltes])
            _sib = len(kept) - _direct
            print(f"Harvested roots: {_direct}/{len(harvested)} usable"
                  + (f" (+{_sib} from jump/call tables)" if _sib else "")
                  + (f", {len(unproven)} not function starts — kept as blocks"
                     if unproven else ""))
        if bogus:
            # Never drop these quietly. A guest dispatching to a non-instruction
            # is itself the finding: the call target was computed from corrupt
            # state, so the fix is upstream and lifting the address cannot help.
            print(f"  ⚠️  DROPPED {len(bogus)} root(s) that are UNRESOLVED IMPORT "
                  f"slots, not code — the guest called an IAT slot's on-disk "
                  f"hint/name RVA because that import is not wired up. Fix the "
                  f"import, not the lift:")
            for va in bogus:
                print(f"       0x{va:x}")
    reached = clo.expand(roots)

    out = []
    for va, synth_size in reached.items():
        if va in idx:
            size, bh, name = idx[va]
        else:
            size = synth_size
            if size <= 0:
                continue
            bh = clo._img(va, size).hex()
            name = f"init_{va:x}"
        out.append((va, va - pe.image_base, size, bh, name))
    return out
