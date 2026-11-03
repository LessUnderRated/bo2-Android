#!/usr/bin/env python3
"""x86c.py <exe> <va_debut> <va_ret> <nom> <témoin> : traduit une fonction x86 (sous-ensemble entier + SSE flottant) en C++.

Registres invites en variables locales, mémoire par win32_mem_read32/write32 (carte plate, barrière d'écriture
comprise), `call` direct = vidage des registres dans State, __remill_function_call, rechargement. Chaque opération
flottante est une opération IEEE par voie, contraction FMA coupée : bit à bit = SSE (vérifié par diff.py).
Refuse toute instruction qu'il ne connaît pas. Sortie : stdout (à inclure dans win32_runtime.cpp).
"""
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

import os
CHECK = os.environ.get("X86C_CHECK") == "1"   # débogage : cases promues doublées en mémoire et comparées à la lecture
G32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
STATE = {r: "s->gpr.r%s.dword" % r[1:] for r in G32}
FLAGSET = {"add", "sub", "and", "or", "xor", "cmp", "test", "inc", "dec", "neg"}


def load_text(exe):
    raw = open(exe, "rb").read()
    pe = struct.unpack_from("<I", raw, 0x3C)[0]
    ns = struct.unpack_from("<H", raw, pe + 6)[0]
    opt = struct.unpack_from("<H", raw, pe + 20)[0]
    base = struct.unpack_from("<I", raw, pe + 52)[0]
    secs = []
    for i in range(ns):
        o = pe + 24 + opt + i * 40
        va, rs, ro = struct.unpack_from("<I", raw, o + 12)[0], struct.unpack_from("<I", raw, o + 16)[0], struct.unpack_from("<I", raw, o + 20)[0]
        secs.append((base + va, raw[ro:ro + rs]))
    return secs


def read_va(secs, va, n):
    for a, d in secs:
        if a <= va < a + len(d):
            return d[va - a:va - a + n]
    raise SystemExit("VA hors image : %x" % va)


class T:
    def __init__(self, cs):
        self.cs = cs
        self.out = []
        self.last_flag = None   # mnémonique du dernier positionneur de drapeaux (ordre linéaire)
        # Cadre de pile promu : esp = F + d, F = esp juste après le premier `sub esp, N`. Les cases [F, F+N) vivent
        # dans cf[] (le cadre est privé : aucun lea/mov ne sort l'adresse d'esp, vérifié), le reste reste en mémoire.
        self.d = None
        self.frame_n = None
        self.d_at = {}
        self.jumps = []

    def reg(self, r):
        n = self.cs.reg_name(r)
        if n in G32:
            return n
        if n.startswith("xmm") and int(n[3:]) < 8:
            return n
        raise SystemExit("registre non géré : %s" % n)

    def addr(self, op):
        m = op.mem
        if self.frame_n is not None and m.base and self.cs.reg_name(m.base) == "ebp" and m.disp < 0:
            raise SystemExit("accès [ebp-%x] avec un cadre promu" % -m.disp)
        if m.segment:
            raise SystemExit("segment non géré")
        if not CHECK and self.slot(op, 1) is not None:
            raise SystemExit("case de cadre lue ou écrite en mémoire à %x : chemin non promu" % self.cur)
        parts = []
        if m.base:
            parts.append(self.reg(m.base))
        if m.index:
            parts.append("%s*%du" % (self.reg(m.index), m.scale))
        if m.disp or not parts:
            parts.append("0x%xu" % (m.disp & 0xFFFFFFFF))
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def slot(self, op, size):
        m = op.mem
        if self.d is None or self.frame_n is None or not m.base or self.cs.reg_name(m.base) != "esp" or m.index:
            return None
        o = self.d + m.disp
        if 0 <= o and o + size <= self.frame_n:
            if o % 4:
                raise SystemExit("case de cadre désalignée : %x" % o)
            return o // 4
        return None

    def src32(self, op):
        if op.type == X86_OP_REG:
            return self.reg(op.reg)
        if op.type == X86_OP_IMM:
            return "0x%xu" % (op.imm & 0xFFFFFFFF)
        if op.type == X86_OP_MEM:
            if op.size != 4:
                raise SystemExit("taille mémoire %d" % op.size)
            k = self.slot(op, 4)
            if k is not None and CHECK:
                return "CK(%d, %s, 0x%x)" % (k, self.addr(op), self.cur)
            return "cf[%d]" % k if k is not None else "rd32(%s)" % self.addr(op)
        raise SystemExit("opérande")

    def dst32(self, op, val):
        if op.type == X86_OP_REG:
            return "%s = %s;" % (self.reg(op.reg), val)
        if op.type == X86_OP_MEM:
            k = self.slot(op, 4)
            if k is not None and CHECK:
                return "{ cf[%d] = %s; wr32(%s, cf[%d]); }" % (k, val, self.addr(op), k)
            return "cf[%d] = %s;" % (k, val) if k is not None else "wr32(%s, %s);" % (self.addr(op), val)
        raise SystemExit("destination")

    def xsrc(self, op, scalar):
        """xmm source : registre, ou chargement mémoire (4 octets si scalaire, 16 sinon)."""
        if op.type == X86_OP_REG:
            return self.reg(op.reg)
        if op.type == X86_OP_MEM:
            k = self.slot(op, 4 if scalar else 16)
            if k is not None and CHECK:
                return "CKX(%d, %s, %d, 0x%x)" % (k, self.addr(op), 1 if scalar else 4, self.cur)
            if k is not None:
                return ("X{{cf[%d], 0u, 0u, 0u}}" % k) if scalar else ("X{{cf[%d], cf[%d], cf[%d], cf[%d]}}" % (k, k + 1, k + 2, k + 3))
            return ("ldss(%s)" if scalar else "ldps(%s)") % self.addr(op)
        raise SystemExit("opérande xmm")

    def emit(self, s):
        self.out.append("    " + s)

    def esp_effect(self, i):
        """Met à jour d après l'instruction i (effet sur esp) ; refuse ce qui ferait sortir l'adresse du cadre."""
        ops, mn = i.operands, i.mnemonic
        r0 = self.cs.reg_name(ops[0].reg) if ops and ops[0].type == X86_OP_REG else None
        for op in ops[1:]:
            if op.type == X86_OP_REG and self.cs.reg_name(op.reg) == "esp" and self.frame_n is not None and mn != "mov" or \
               (mn == "mov" and r0 != "esp" and op.type == X86_OP_REG and self.cs.reg_name(op.reg) == "esp" and self.frame_n is not None):
                raise SystemExit("adresse d'esp copiée à %x : cadre non promouvable" % i.address)
        if mn == "lea" and r0 != "esp" and ops[1].mem.base and self.cs.reg_name(ops[1].mem.base) == "esp" and self.frame_n is not None:
            raise SystemExit("lea sur esp à %x : cadre non promouvable" % i.address)
        if mn == "push":
            if self.d is not None: self.d -= 4
        elif mn == "pop":
            if r0 == "esp": self.d = None
            elif self.d is not None: self.d += 4
        elif r0 == "esp" and mn == "sub" and ops[1].type == X86_OP_IMM:
            if self.frame_n is None:
                self.frame_n, self.d = ops[1].imm, 0
                self.emit("F = esp; for (uint32_t k_ = 0; k_ < %du; k_++) cf[k_] = rd32(F + 4u * k_);" % (ops[1].imm // 4))
            elif self.d is not None:
                self.d -= ops[1].imm
        elif r0 == "esp" and mn == "add" and ops[1].type == X86_OP_IMM:
            if self.d is not None: self.d += ops[1].imm
        elif r0 == "esp" and mn == "lea" and ops[1].mem.base and self.cs.reg_name(ops[1].mem.base) == "esp" and not ops[1].mem.index:
            if self.d is not None: self.d += ops[1].mem.disp
        elif r0 == "esp" and mn not in ("cmp", "test", "push"):
            self.d = None
        elif mn == "leave":
            self.d = None

    def one(self, i, labels):
        self.d_at[i.address] = self.d
        if i.mnemonic.startswith("j") and i.mnemonic != "jmp" or i.mnemonic == "jmp":
            self.jumps.append((i.address, i.operands[0].imm, self.d))
        self.one_(i, labels)
        self.esp_effect(i)

    def one_(self, i, labels):
        self.cur = i.address
        ops, mn = i.operands, i.mnemonic
        if i.address in labels:
            self.out.append("L_%x:" % i.address)
        e = self.emit
        if mn in ("prefetcht0", "prefetcht1", "prefetcht2", "prefetchnta", "nop"):
            return
        if mn == "mov":
            e(self.dst32(ops[0], self.src32(ops[1])))
        elif mn == "lea":
            e("%s = %s;" % (self.reg(ops[0].reg), self.addr(ops[1])))
        elif mn in ("add", "sub", "and", "or", "xor", "cmp", "test"):
            a, b = self.src32(ops[0]), self.src32(ops[1])
            if mn in ("xor",) and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg:
                b = a
            op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^", "cmp": "-", "test": "&"}[mn]
            e("fa = %s; fb = %s; fr = fa %s fb;" % (a, b, op))
            if mn not in ("cmp", "test"):
                e(self.dst32(ops[0], "fr"))
        elif mn in ("inc", "dec"):
            a = self.src32(ops[0])
            e("fr = %s %s 1u;" % (a, "+" if mn == "inc" else "-"))
            e(self.dst32(ops[0], "fr"))
        elif mn == "push":
            e("{ const uint32_t v_ = %s; esp -= 4u; wr32(esp, v_); }" % self.src32(ops[0]))
        elif mn == "pop":
            e("%s = rd32(esp); esp += 4u;" % self.reg(ops[0].reg))
        elif mn.startswith("j"):
            tgt = "L_%x" % ops[0].imm
            if mn == "jmp":
                e("goto %s;" % tgt)
                return
            lf = self.last_flag
            cond = {
                "jne": "fr != 0u", "je": "fr == 0u", "js": "(int32_t)fr < 0", "jns": "(int32_t)fr >= 0",
            }.get(mn)
            if cond is None:
                if lf not in ("cmp", "sub"):
                    raise SystemExit("%s après %s non géré à %x" % (mn, lf, i.address))
                cond = {"jbe": "fa <= fb", "ja": "fa > fb", "jb": "fa < fb", "jae": "fa >= fb",
                        "jl": "(int32_t)fa < (int32_t)fb", "jge": "(int32_t)fa >= (int32_t)fb",
                        "jle": "(int32_t)fa <= (int32_t)fb", "jg": "(int32_t)fa > (int32_t)fb"}[mn]
            if mn in ("jne", "je", "js", "jns") and lf not in FLAGSET:
                raise SystemExit("%s après %s à %x" % (mn, lf, i.address))
            e("if (%s) goto %s;" % (cond, tgt))
        elif mn == "call":
            if ops[0].type != X86_OP_IMM:
                raise SystemExit("call indirect à %x" % i.address)
            e("esp -= 4u; wr32(esp, 0x%xu);" % (i.address + i.size))
            e("FLUSH(); mem = __remill_function_call(*s, 0x%xu, mem); RELOAD();" % ops[0].imm)
        elif mn == "ret":
            e("esp += 4u; goto L_fin;")
        # ── SSE ──
        elif mn == "movss":
            d, sv = ops
            if d.type == X86_OP_MEM:
                k = self.slot(d, 4)
                if k is not None and CHECK:
                    e("wr32(%s, %s.u[0]);" % (self.addr(d), self.reg(sv.reg)))
                e("cf[%d] = %s.u[0];" % (k, self.reg(sv.reg)) if k is not None else "wr32(%s, %s.u[0]);" % (self.addr(d), self.reg(sv.reg)))
            elif sv.type == X86_OP_MEM:
                e("%s = %s;" % (self.reg(d.reg), self.xsrc(sv, True)))
            else:
                e("%s.u[0] = %s.u[0];" % (self.reg(d.reg), self.reg(sv.reg)))
        elif mn in ("movaps", "movups"):
            d, sv = ops
            if d.type == X86_OP_MEM:
                k = self.slot(d, 16)
                x = self.reg(sv.reg)
                if k is not None and CHECK:
                    e("stps(%s, %s);" % (self.addr(d), x))
                e(" ".join("cf[%d] = %s.u[%d];" % (k + j, x, j) for j in range(4)) if k is not None else "stps(%s, %s);" % (self.addr(d), x))
            else:
                e("%s = %s;" % (self.reg(d.reg), self.xsrc(sv, False)))
        elif mn in ("addps", "subps", "mulps", "divps", "minps", "maxps", "andps", "orps", "xorps", "andnps"):
            d = self.reg(ops[0].reg)
            e("%s = x_%s(%s, %s);" % (d, mn[:-2], d, self.xsrc(ops[1], False)))
        elif mn in ("addss", "subss", "mulss", "divss", "minss", "maxss"):
            d = self.reg(ops[0].reg)
            e("%s.u[0] = x_%s(%s, %s).u[0];" % (d, mn[:-2], d, self.xsrc(ops[1], True)))
        elif mn == "shufps":
            d = self.reg(ops[0].reg)
            e("%s = x_shuf(%s, %s, 0x%x);" % (d, d, self.xsrc(ops[1], False), ops[2].imm))
        elif mn in ("unpcklps", "unpckhps", "movlhps", "movhlps"):
            d = self.reg(ops[0].reg)
            e("%s = x_%s(%s, %s);" % (d, mn, d, self.xsrc(ops[1], False)))
        else:
            raise SystemExit("instruction non gérée à %x : %s %s" % (i.address, mn, i.op_str))
        if mn in FLAGSET:
            self.last_flag = mn


def main():
    exe, start, ret, name, tag = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4], sys.argv[5]
    secs = load_text(exe)
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    cs.detail = True
    code = read_va(secs, start, ret + 1 - start)
    ins = list(cs.disasm(code, start))
    if not ins or ins[-1].address != ret or ins[-1].mnemonic != "ret":
        raise SystemExit("le désassemblage ne finit pas sur ret à %x" % ret)
    labels = {i.operands[0].imm for i in ins if i.mnemonic.startswith("j")}
    for t in labels:
        if not start <= t <= ret:
            raise SystemExit("saut hors fonction : %x" % t)
    t = T(cs)
    for i in ins:
        t.one(i, labels)
    for src, dst, d in t.jumps:
        if t.d_at.get(dst) != d:
            raise SystemExit("esp incohérent sur le saut %x -> %x (%s / %s)" % (src, dst, d, t.d_at.get(dst)))
    print("// Généré par scripts/x86c.py depuis 0x%x-0x%x (%d instructions). Ne pas éditer à la main." % (start, ret, len(ins)))
    print('extern "C" __attribute__((used)) void* %s(State* s, uint32_t pc, void* mem) {' % name)
    print('    _Pragma("clang fp contract(off)")')
    print("    (void)pc;")
    print("    using namespace x86c;")
    print("    uint32_t " + ", ".join("%s = %s" % (r, STATE[r]) for r in G32) + ";")
    print("    X " + ", ".join("xmm%d = x_get(s, %d)" % (k, k) for k in range(8)) + ";")
    print("    uint32_t fa = 0, fb = 0, fr = 0; (void)fa; (void)fb; (void)fr;")
    print("    uint32_t F = 0, cf[%d]; (void)F;" % max(1, (t.frame_n or 4) // 4))
    print("#define FLUSH() do { " + " ".join("%s = %s;" % (STATE[r], r) for r in G32)
          + " " + " ".join("x_set(s, %d, xmm%d);" % (k, k) for k in range(8)) + " } while (0)")
    print("#define RELOAD() do { " + " ".join("%s = %s;" % (r, STATE[r]) for r in G32)
          + " " + " ".join("xmm%d = x_get(s, %d);" % (k, k) for k in range(8)) + " } while (0)")
    print("\n".join(t.out))
    print("L_fin:")
    print('    { static const bool probe = bo2_getenv("BO2_SND_PROBE") != nullptr; static uint64_t calls = 0;')
    print('      if (probe && (++calls <= 3 || (calls %% 100000) == 0)) fprintf(stderr, "[%s] call #%%llu\\n", (unsigned long long)calls); }' % tag)
    print("    FLUSH();")
    print("#undef FLUSH")
    print("#undef RELOAD")
    print("    return mem;")
    print("}")


if __name__ == "__main__":
    main()
