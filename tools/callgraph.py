#!/usr/bin/env python3
"""Graphe d'appels DIRECT de t6sp.exe, construit hors ligne.

Pourquoi ce fichier existe : `tools/disasm.py --xref` ne trouve que les
operandes IMMEDIATES (`push 0xbb98f4`). Il rend « 0 reference » pour une
fonction pourtant appelee par `call rel32` — j'ai perdu du temps la-dessus le
2026-09-17 sur 0x6cd5e0, qui a en fait quatre appelants directs.

    tools/callgraph.py callers 0x6cd5e0          qui appelle cette fonction
    tools/callgraph.py up      0x0059e1a0 [prof] remonte la chaine d'appel
    tools/callgraph.py path    0x00601000 0x6cd5e0   existe-t-il un chemin ?
    tools/callgraph.py callees 0x006cd5e0        qui elle appelle

Limite assumee : APPELS DIRECTS SEULEMENT. Un `call [eax+0x1c]` (vtable) est
invisible — c'est exactement comme ca que 0x703980 est atteinte. Une absence de
chemin ici n'est donc PAS une preuve d'absence.
"""
import struct, sys, bisect, collections

EXE = '/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe'
ROOTS = __file__.rsplit('/', 1)[0] + '/lift_roots_ref40k.txt'


def load():
    d = open(EXE, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    text = None
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        name = d[o:o + 8].rstrip(b'\0').decode('latin1')
        _vsz, va, rsz, ra = struct.unpack_from('<IIII', d, o + 8)
        if name == '.text':
            text = (base + va, d[ra:ra + rsz])
    return text


def starts():
    out = []
    for line in open(ROOTS):
        line = line.split('#')[0].strip()
        if not line:
            continue
        tok = line.split()[0]
        try:
            out.append(int(tok, 16))
        except ValueError:
            pass
    out.sort()
    return out


def build():
    tva, blob = load()
    fs = starts()
    edges = collections.defaultdict(set)      # appelant -> {appele}
    rev = collections.defaultdict(set)        # appele  -> {appelant}
    for i, b in enumerate(blob):
        if b != 0xe8 or i + 5 > len(blob):
            continue
        rel = struct.unpack_from('<i', blob, i + 1)[0]
        tgt = (tva + i + 5 + rel) & 0xffffffff
        site = tva + i
        k = bisect.bisect_right(fs, site) - 1
        if k < 0:
            continue
        edges[fs[k]].add(tgt)
        rev[tgt].add((fs[k], site))
    return fs, edges, rev


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    va = int(sys.argv[2], 16)
    fs, edges, rev = build()

    if cmd == 'callers':
        for f, site in sorted(rev.get(va, ())):
            print(f"  0x{f:08x}  (site 0x{site:08x})")
        print(f"{len(rev.get(va, ()))} appelant(s) direct(s) de 0x{va:08x}")
    elif cmd == 'callees':
        k = bisect.bisect_right(fs, va) - 1
        f = fs[k]
        for t in sorted(edges.get(f, ())):
            print(f"  0x{t:08x}")
        print(f"{len(edges.get(f, ()))} appel(s) direct(s) depuis 0x{f:08x}")
    elif cmd == 'up':
        depth = int(sys.argv[3]) if len(sys.argv) > 3 else 6
        seen, level = {va}, [va]
        for d_ in range(depth):
            nxt = []
            for t in level:
                for f, site in sorted(rev.get(t, ())):
                    if f in seen:
                        continue
                    seen.add(f)
                    nxt.append(f)
                    print(f"{'  ' * d_}0x{f:08x} -> 0x{t:08x}  (site 0x{site:08x})")
            if not nxt:
                print(f"{'  ' * d_}(plus d'appelant direct — probablement indirect/vtable)")
                break
            level = nxt
    elif cmd == 'path':
        dst = int(sys.argv[3], 16)
        q, prev = collections.deque([va]), {va: None}
        while q:
            cur = q.popleft()
            if cur == dst or dst in edges.get(cur, ()):
                chain, n = [], dst if cur != dst else cur
                if cur != dst:
                    prev[dst] = cur
                while n is not None:
                    chain.append(f"0x{n:08x}")
                    n = prev[n]
                print(" -> ".join(reversed(chain)))
                return 0
            for t in edges.get(cur, ()):
                k = bisect.bisect_right(fs, t) - 1
                tf = fs[k] if k >= 0 else t
                if tf not in prev:
                    prev[tf] = cur
                    q.append(tf)
        print("aucun chemin DIRECT (les appels de vtable sont invisibles ici)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
