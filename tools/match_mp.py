#!/usr/bin/env python3
"""Map a function address in t6sp.exe onto the same function in t6mp.exe.

WHY THIS EXISTS. Multiplayer is a different executable — there is no in-process
route from t6sp.exe to it (see docs/handoff/MP-01). Porting to t6mp.exe reuses
the entire runtime, but the runtime names guest functions by ADDRESS: ~150
hard-coded VAs across scripts/build_only.sh's patch_redirect line and
runtime/**. Every one of them is a t6sp address and every one is wrong for
t6mp.exe.

So the question that decides whether the MP port is mechanical or hopeless is a
number: what fraction of those 150 addresses can be relocated automatically?
This script answers it, and it answers it with evidence per address rather than
a single percentage — a match that cannot say WHY it matched is not usable for
patching a binary.

METHOD — string-anchored, then confirmed by shape.

  1. Harvest every C string in both images and index which .text sites reference
     it (a 4-byte little-endian dword equal to the string's VA).
  2. Approximate function starts as the union of all `call rel32` targets plus
     every offset that follows int3 padding. MSVC pads with 0xcc, so this is
     accurate enough; it does not need to be perfect because step 4 scores.
  3. For each function, collect the SET of strings it references.
  4. A t6sp function maps to the t6mp function maximising a Jaccard-like score
     over those sets, with rare strings weighted far above common ones. A string
     referenced from one place in each image is nearly a proof by itself.
  5. Confirm with a normalised-opcode-stream similarity: disassemble both, keep
     mnemonics and operand SHAPES, drop every address-like immediate. Two builds
     of the same source differ in addresses and in nothing else.

WHAT IT DELIBERATELY DOES NOT DO. It does not guess. A function with no string
references and no confident shape match is reported as UNMATCHED, loudly, with
its name. Nine interposes in this repo were dead code for weeks because a silent
failure is indistinguishable from a thing that never happens (docs/GUIDE.md §9);
a matcher that quietly returns a plausible wrong address would be that same bug
with a bigger blast radius.

USAGE
    tools/match_mp.py --self-test              # map t6sp onto ITSELF; must be 100%
    tools/match_mp.py 0x479f70 0x8062a0        # map specific VAs
    tools/match_mp.py --interposes             # every VA the build hard-codes
    tools/match_mp.py --interposes --json out.json

❌ THE SELF-TEST FAILS, AND THE NUMBER IS THE POINT (2026-08-02).
Mapping t6sp.exe onto ITSELF — the trivial case, where every answer is the
input — this scores:

    26/147 confident, 76 weak, 45 unmatched
    95/147 land on themselves

So string anchoring alone reaches at best ~65% even when the two images are the
SAME FILE, and only 18% of those are confident. It must not be used to relocate
addresses for a t6mp.exe build in this state: a "weak" match here is a wrong
address in a patch_redirect line, which produces a shim that silently interposes
the wrong function — the most expensive failure mode this repo has.

WHY IT FAILS, from the misses themselves: the interposes this project cares
about are mostly small helpers with NO string references at all —
0x5c02f0, 0x7426e0, 0x772300, 0x78b1c0, 0x7ada50, 0x8e7bc0, 0x925520, 0x92e0f0,
0x9b70b0, 0x9ba7f0, 0x9bb3a0, 0x9bb620 … all report "no anchoring strings in
this function". Networking, audio and codec leaves do not print messages.

WHAT WOULD ACTUALLY WORK, for whoever picks this up: the missing step is
CALL-GRAPH PROPAGATION, which is how BinDiff really works. Use the confident
string-anchored matches as SEEDS, then iterate: two functions match if their
callers match and they are called from the same call-site index, or if their
callee sequences match. A leaf with no strings is identified by its neighbours,
never by itself. The seeds this already produces are enough to start that.
"""
import argparse
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = "/Users/Shared/Call of Duty - Black Ops 2"
SP = os.path.join(GAME, "t6sp.exe")
MP = os.path.join(GAME, "t6mp.exe")

MIN_STR = 5          # shorter strings are noise ("%s", "on", "mp")
MAX_STR = 400


# ---------------------------------------------------------------- image ----
class Image:
    """Just enough PE for this job: sections mapped at their virtual addresses."""

    def __init__(self, path):
        self.path = path
        raw = open(path, "rb").read()
        pe = struct.unpack_from("<I", raw, 0x3C)[0]
        assert raw[pe:pe + 4] == b"PE\0\0", f"{path}: not a PE"
        nsec = struct.unpack_from("<H", raw, pe + 6)[0]
        optsz = struct.unpack_from("<H", raw, pe + 20)[0]
        self.base = struct.unpack_from("<I", raw, pe + 24 + 28)[0]
        self.sections = []           # (va, bytes, name)
        off = pe + 24 + optsz
        for i in range(nsec):
            e = off + i * 40
            name = raw[e:e + 8].rstrip(b"\0").decode("latin1")
            vsz = struct.unpack_from("<I", raw, e + 8)[0]
            rva = struct.unpack_from("<I", raw, e + 12)[0]
            rsz = struct.unpack_from("<I", raw, e + 16)[0]
            ptr = struct.unpack_from("<I", raw, e + 20)[0]
            data = raw[ptr:ptr + min(rsz, vsz)]
            self.sections.append((self.base + rva, data, name))
        self.text_va, self.text, _ = next(
            (s for s in self.sections if s[2] == ".text"))
        self.text_end = self.text_va + len(self.text)

    def read(self, va, n):
        for base, data, _ in self.sections:
            if base <= va < base + len(data):
                return data[va - base: va - base + n]
        return b""

    def cstr(self, va, limit=MAX_STR):
        b = self.read(va, limit)
        i = b.find(b"\0")
        return b[:i] if i >= 0 else b


# --------------------------------------------------------------- strings ----
_PRINTABLE = bytes(range(0x20, 0x7F)) + b"\t"


def harvest_strings(img):
    """content -> VA, for strings that appear exactly once in the image.

    Ambiguous contents (the same text at several addresses) are dropped: they
    cannot anchor anything, and keeping them would let a wrong pairing score.
    """
    seen = collections.defaultdict(list)
    ok = set(_PRINTABLE)
    for base, data, name in img.sections:
        if name in (".text",):        # code section holds few real literals
            continue
        n = len(data)
        i = 0
        while i < n:
            if data[i] in ok:
                j = i
                while j < n and data[j] in ok and j - i < MAX_STR:
                    j += 1
                if j < n and data[j] == 0 and j - i >= MIN_STR:
                    seen[data[i:j]].append(base + i)
                i = j + 1
            else:
                i += 1
    return {k: v[0] for k, v in seen.items() if len(v) == 1}


def string_refs(img, str_vas):
    """VA-of-string -> [site VAs in .text]. One linear pass over .text."""
    want = set(str_vas)
    out = collections.defaultdict(list)
    t, tva = img.text, img.text_va
    for off in range(0, len(t) - 4):
        v = t[off] | (t[off + 1] << 8) | (t[off + 2] << 16) | (t[off + 3] << 24)
        if v in want:
            out[v].append(tva + off)
    return out


# ------------------------------------------------------- function starts ----
def function_starts(img):
    """Union of `call rel32` targets and post-int3-padding offsets."""
    starts = set()
    t, tva, tend = img.text, img.text_va, img.text_end
    n = len(t)
    for off in range(0, n - 5):
        if t[off] != 0xE8:
            continue
        rel = struct.unpack_from("<i", t, off + 1)[0]
        tgt = tva + off + 5 + rel
        if tva <= tgt < tend:
            starts.add(tgt)
    # After a run of >=2 int3, the next non-int3 byte begins something.
    run = 0
    for off in range(n):
        if t[off] == 0xCC:
            run += 1
        else:
            if run >= 2:
                starts.add(tva + off)
            run = 0
    return sorted(starts)


def enclosing(starts, va):
    """Largest function start <= va."""
    import bisect
    i = bisect.bisect_right(starts, va) - 1
    return starts[i] if i >= 0 else None


def next_start(starts, va):
    import bisect
    i = bisect.bisect_right(starts, va)
    return starts[i] if i < len(starts) else None


# ---------------------------------------------------------- normalisation ----
def norm_shape(img, va, limit=256):
    """Opcode stream with every address-like immediate erased.

    Two builds of the same C++ differ in addresses and register allocation is
    identical, so this is a strong fingerprint. Returns None when capstone is
    unavailable rather than pretending to have checked.
    """
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    except ImportError:
        return None
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    code = img.read(va, limit)
    out = []
    for ins in md.disasm(code, va):
        if ins.mnemonic in ("int3", "nop"):
            break
        ops = re.sub(r"0x[0-9a-f]{5,8}", "A", ins.op_str)
        ops = re.sub(r"\b\d{5,}\b", "A", ops)
        out.append(ins.mnemonic + " " + ops)
        if ins.mnemonic == "ret":
            break
    return tuple(out)


def shape_sim(a, b):
    if not a or not b:
        return 0.0
    n = min(len(a), len(b))
    same = sum(1 for i in range(n) if a[i] == b[i])
    return same / max(len(a), len(b))


# ---------------------------------------------------------------- match ----
class Matcher:
    def __init__(self, src_path, dst_path):
        sys.stderr.write(f"[match] loading {os.path.basename(src_path)}\n")
        self.src = Image(src_path)
        sys.stderr.write(f"[match] loading {os.path.basename(dst_path)}\n")
        self.dst = Image(dst_path)
        sys.stderr.write("[match] harvesting strings\n")
        self.s_str = harvest_strings(self.src)
        self.d_str = harvest_strings(self.dst)
        # Only contents present, uniquely, in BOTH images can anchor.
        self.common = set(self.s_str) & set(self.d_str)
        sys.stderr.write(f"[match] {len(self.s_str)} / {len(self.d_str)} unique strings, "
                         f"{len(self.common)} common\n")
        sys.stderr.write("[match] indexing .text references (this is the slow part)\n")
        self.s_refs = string_refs(self.src, {self.s_str[c] for c in self.common})
        self.d_refs = string_refs(self.dst, {self.d_str[c] for c in self.common})
        sys.stderr.write("[match] deriving function starts\n")
        self.s_starts = function_starts(self.src)
        self.d_starts = function_starts(self.dst)
        sys.stderr.write(f"[match] {len(self.s_starts)} / {len(self.d_starts)} function starts\n")
        # function -> set(common string content)   and   content -> set(functions)
        self.s_fn2str, self.s_str2fn = self._invert(self.s_str, self.s_refs, self.s_starts)
        self.d_fn2str, self.d_str2fn = self._invert(self.d_str, self.d_refs, self.d_starts)

    def _invert(self, strtab, refs, starts):
        fn2str = collections.defaultdict(set)
        str2fn = collections.defaultdict(set)
        for content in self.common:
            sva = strtab[content]
            for site in refs.get(sva, ()):
                fn = enclosing(starts, site)
                if fn is not None:
                    fn2str[fn].add(content)
                    str2fn[content].add(fn)
        return fn2str, str2fn

    def match(self, va):
        """-> (dst_va, confidence, why) or (None, 0.0, reason)."""
        fn = enclosing(self.s_starts, va)
        if fn is None:
            return None, 0.0, "no enclosing function start in the source image"
        interior = va - fn
        strs = self.s_fn2str.get(fn, set())
        cands = collections.Counter()
        for c in strs:
            # weight: a string used once on each side is worth far more than a
            # format string used in two hundred functions.
            w = 1.0 / (len(self.s_str2fn[c]) * max(1, len(self.d_str2fn.get(c, ()))))
            for d in self.d_str2fn.get(c, ()):
                cands[d] += w
        why = []
        best = None
        if cands:
            best, score = cands.most_common(1)[0]
            runner = cands.most_common(2)[1][1] if len(cands) > 1 else 0.0
            why.append(f"{len(strs)} strings, top score {score:.3f} vs runner-up {runner:.3f}")
        else:
            score = runner = 0.0
            why.append("no anchoring strings in this function")

        # Shape confirmation, on the FUNCTION START (interiors shift).
        sh_s = norm_shape(self.src, fn)
        if best is not None:
            sim = shape_sim(sh_s, norm_shape(self.dst, best))
            why.append(f"shape {sim:.2f}")
        else:
            sim = 0.0
            # Last resort: a pure shape sweep is O(n) over 100k starts — only do
            # it for the small hand-picked set, and only when nothing else works.
            cand2 = []
            if sh_s and len(sh_s) >= 8:
                for d in self.d_starts:
                    s2 = shape_sim(sh_s, norm_shape(self.dst, d, limit=len(sh_s) * 8 + 32))
                    if s2 > 0.95:
                        cand2.append((s2, d))
                cand2.sort(reverse=True)
            if len(cand2) == 1:
                best, sim = cand2[0][1], cand2[0][0]
                why.append(f"unique shape-only match {sim:.2f}")
            elif cand2:
                why.append(f"{len(cand2)} equally-shaped candidates — ambiguous, refusing")
                return None, 0.0, "; ".join(why)
            else:
                return None, 0.0, "; ".join(why)

        # Confidence: strings dominate, shape confirms. Anything below 0.6 is
        # reported as a suggestion, never as an answer.
        margin = 1.0 if runner == 0 else min(1.0, (score - runner) / max(score, 1e-9))
        conf = 0.55 * margin * (1.0 if strs else 0.0) + 0.45 * sim
        if best is not None and interior:
            why.append(f"interior +0x{interior:x} carried over unchecked")
        return (best + interior if best is not None else None), conf, "; ".join(why)


# ----------------------------------------------------------------- main ----
def interpose_vas():
    """Every guest VA the build hard-codes, straight out of build_only.sh."""
    src = open(os.path.join(HERE, "..", "scripts", "build_only.sh")).read()
    vas = collections.OrderedDict()
    for m in re.finditer(r"_sub_([0-9a-f]{6})=_(\w+)", src):
        vas.setdefault(int(m.group(1), 16), m.group(2))
    return vas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vas", nargs="*")
    ap.add_argument("--interposes", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--json")
    a = ap.parse_args()

    dst = SP if a.self_test else MP
    m = Matcher(SP, dst)

    if a.interposes or a.self_test:
        targets = interpose_vas()
    else:
        targets = {int(v, 16 if v.startswith("0x") else 16): "" for v in a.vas}
    if a.vas:
        targets = {int(v, 16): targets.get(int(v, 16), "") for v in a.vas}

    ok = amb = 0
    rows = []
    for va, name in targets.items():
        d, conf, why = m.match(va)
        if d is not None and conf >= 0.6:
            ok += 1
            mark = "OK  "
        elif d is not None:
            amb += 1
            mark = "WEAK"
        else:
            mark = "MISS"
        exact = " (exact)" if a.self_test and d == va else (
            " ⚠️ SELF-TEST MISMATCH" if a.self_test else "")
        print(f"{mark} 0x{va:06x} {name:<24} -> "
              f"{('0x%06x' % d) if d else '-':<10} conf={conf:.2f}{exact}  {why}")
        rows.append({"src": va, "name": name, "dst": d, "conf": conf, "why": why})

    n = len(targets)
    print(f"\n=== {ok}/{n} confident, {amb} weak, {n - ok - amb} unmatched ===")
    if a.self_test:
        bad = [r for r in rows if r["dst"] != r["src"]]
        print(f"=== self-test: {n - len(bad)}/{n} land on themselves ===")
        for r in bad[:20]:
            print(f"    0x{r['src']:06x} -> {r['dst'] and hex(r['dst'])}  {r['why']}")
    if a.json:
        import json
        json.dump(rows, open(a.json, "w"), indent=1)
        print(f"wrote {a.json}")


if __name__ == "__main__":
    main()
