#!/usr/bin/env python3
"""Parse a T6 (Black Ops 2) compiled GSC blob out of a decompressed zone.

    tools/gsc_script.py <zone.bin> --name maps/_empgrenade.gsc
    tools/gsc_script.py <zone.bin> --name maps/_empgrenade.gsc --at 0x2d8

Why this exists: the VM's instruction pointer resolves to `script.gsc+OFFSET`
in the tracer, and every attempt to turn that offset into a NAME has so far
gone through a 10-minute instrumented run. The blob carries its own exports
table and its own string-table fixups, so both the enclosing function and the
16-bit key at any code offset are readable offline, in milliseconds.

Header (offsets from the `\\x80GSC\\r\\n\\x00\\x06` magic):

    0x08 u32 source crc          0x24 u32 fixup offset
    0x0c u32 include offset      0x28 u32 profile offset
    0x10 u32 animtree offset     0x2c u32 cseg size
    0x14 u32 cseg offset         0x30 u16 name (string offset)
    0x18 u32 stringtablefixup    0x32 u16 stringtablefixup count
    0x1c u32 exports offset      0x34 u16 exports count
    0x20 u32 imports offset      0x36 u16 imports count

An export is 12 bytes: {u32 crc, u32 offset, u16 name, u8 params, u8 flags}.
A string-table fixup is {u16 name, u8 nrefs, u8 type} followed by nrefs u32
addresses — so it maps a NAME to every code address that references it, which
is exactly the direction needed to answer "what key does the opcode at 0x2d8
push?".
"""
import argparse
import struct
import sys

MAGIC = b"\x80GSC\x0d\x0a\x00\x06"


def cstr(d, off):
    e = d.find(b"\0", off)
    return d[off:e].decode("latin1", "replace") if e > off else ""


class Script:
    def __init__(self, data, base):
        self.d = data
        self.b = base
        u32 = lambda o: struct.unpack_from("<I", data, base + o)[0]
        u16 = lambda o: struct.unpack_from("<H", data, base + o)[0]
        self.crc = u32(0x08)
        self.include_off = u32(0x0C)
        self.animtree_off = u32(0x10)
        self.cseg_off = u32(0x14)
        self.stf_off = u32(0x18)
        self.exports_off = u32(0x1C)
        self.imports_off = u32(0x20)
        self.fixup_off = u32(0x24)
        self.cseg_size = u32(0x2C)
        self.name_off = u16(0x30)
        self.stf_n = u16(0x32)
        self.exports_n = u16(0x34)
        self.imports_n = u16(0x36)
        self.name = cstr(data, base + self.name_off)

    def s(self, off):
        return cstr(self.d, self.b + off)

    def exports(self):
        out = []
        for i in range(self.exports_n):
            o = self.b + self.exports_off + i * 12
            crc, off, nm, np, fl = struct.unpack_from("<IIHBB", self.d, o)
            out.append((off, self.s(nm), np, fl, crc))
        return sorted(out)

    def fixups(self):
        """[(name, type, [code offsets])] from the string-table fixup section."""
        out, o = [], self.b + self.stf_off
        for _ in range(self.stf_n):
            nm, n, ty = struct.unpack_from("<HBB", self.d, o)
            o += 4
            refs = list(struct.unpack_from("<%dI" % n, self.d, o)) if n else []
            o += 4 * n
            out.append((self.s(nm), ty, refs))
        return out

    def imports(self):
        """[(name, namespace, params, flags, [call sites])].

        Une entree d'import fait 8 octets — {u16 nom, u16 espace, u16 nrefs,
        u8 params, u8 flags} — suivie de nrefs adresses u32. Comme pour les
        fixups de chaines, la table va du NOM vers ses sites d'appel, donc
        trier les sites par offset reconstitue l'ordre des appels d'une
        fonction sans desassembler le bytecode.
        """
        out, o = [], self.b + self.imports_off
        for _ in range(self.imports_n):
            nm, ns, n, np, fl = struct.unpack_from("<HHHBB", self.d, o)
            o += 8
            refs = list(struct.unpack_from("<%dI" % n, self.d, o)) if n else []
            o += 4 * n
            out.append((self.s(nm), self.s(ns), np, fl, refs))
        return out

    def enclosing(self, off):
        best = None
        for eoff, nm, np, fl, _ in self.exports():
            if eoff <= off and (best is None or eoff > best[0]):
                best = (eoff, nm, np, fl)
        return best


def find(data, name):
    want = name.encode() + b"\0"
    for m in _iter_magic(data):
        try:
            s = Script(data, m)
        except struct.error:
            continue
        if s.name == name:
            return s
    # fall back: locate the name then walk back to the nearest magic
    i = data.find(want)
    if i < 0:
        return None
    for m in _iter_magic(data):
        if m < i < m + 0x100000:
            last = m
    return Script(data, last) if "last" in dir() else None


def _iter_magic(data):
    o = 0
    while True:
        i = data.find(MAGIC, o)
        if i < 0:
            return
        yield i
        o = i + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("zone")
    ap.add_argument("--name", required=True)
    ap.add_argument("--at", help="code offset, e.g. 0x2d8")
    ap.add_argument("--list", action="store_true", help="list every script in the zone")
    # ⚠️ --at ne montre que les fixups a ±0x20 de UNE adresse. Pour lire la FORME
    #    d'une fonction (ses constantes dans l'ordre ou le code les pousse), il faut
    #    la liste complete triee par offset : c'est ce que --strings donne, et c'est
    #    le seul moyen hors ligne de reconstituer un enchainement de script.
    ap.add_argument("--strings", action="store_true",
                    help="dump every string fixup sorted by code offset")
    ap.add_argument("--range", help="limit --strings to LO:HI, e.g. 0xad0:0xc5c")
    a = ap.parse_args()

    data = open(a.zone, "rb").read()
    if a.list:
        for m in _iter_magic(data):
            try:
                print("0x%08x  %s" % (m, Script(data, m).name))
            except struct.error:
                pass
        return

    s = find(data, a.name)
    if not s:
        sys.exit("%s not found in %s" % (a.name, a.zone))
    print("%s  blob at 0x%08x  crc 0x%08x" % (s.name, s.b, s.crc))
    print("  cseg 0x%x..0x%x (%d bytes)  exports %d  imports %d  stringfixups %d"
          % (s.cseg_off, s.cseg_off + s.cseg_size, s.cseg_size,
             s.exports_n, s.imports_n, s.stf_n))
    print("\n  exports:")
    for off, nm, np, fl, _ in s.exports():
        print("    0x%04x  %-40s params=%d flags=0x%02x" % (off, nm, np, fl))

    if a.strings:
        lo, hi = 0, 1 << 30
        if a.range:
            lo, hi = (int(x, 0) for x in a.range.split(":"))
        rows = []
        for nm, ty, refs in s.fixups():
            for r in refs:
                if lo <= r < hi:
                    rows.append((r, "str:%d" % ty, nm))
        for nm, ns, np, fl, refs in s.imports():
            for r in refs:
                if lo <= r < hi:
                    rows.append((r, "CALL", ("%s::%s" % (ns, nm) if ns else nm)
                                 + "(%d)" % np))
        rows.sort()
        print("\n  string fixups by code offset (%d in range):" % len(rows))
        cur = None
        for r, ty, nm in rows:
            enc = s.enclosing(r)
            fn = enc[1] if enc else "?"
            if fn != cur:
                print("    --- %s ---" % fn)
                cur = fn
            print("    0x%04x  %-7s %s" % (r, ty, nm))
        return

    if a.at:
        at = int(a.at, 0)
        enc = s.enclosing(at)
        print("\n  offset 0x%04x is inside %s (starts 0x%04x)"
              % (at, enc[1] if enc else "?", enc[0] if enc else 0))
        print("  string-table fixups referencing addresses near it:")
        for nm, ty, refs in s.fixups():
            near = [r for r in refs if abs(r - at) <= 0x20]
            if near:
                print("    %-34s type=%d at %s" % (nm, ty, ", ".join("0x%04x" % r for r in near)))


if __name__ == "__main__":
    main()
