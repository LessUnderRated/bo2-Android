#!/usr/bin/env python3
"""Carte complete des `call rel32` du .text de t6sp.exe.

POURQUOI CET OUTIL EXISTE (2026-09-19, P561) — deux pieges payes le meme jour :

1. `tools/disasm.py --xref <va>` ne voit que les immediats ABSOLUS (tables de
   saut, pointeurs en donnees). Il ne decode PAS `call rel32`, donc il repond
   « 0 reference » a la question « qui appelle X ». Controle : `--xref 0x6021a0`
   rend 0, alors que cette fonction a 14 sites d'appel. Le bon mode est
   `--calls`.

2. Un symbole `sub_XXXXXX` du lift N'EST PAS une frontiere de fonction. remill
   lifte chaque corps de `switch` comme un `sub_`. Exemple mesure :
   `sub_87eca7` est le cas #7 d'un switch a 12 branches dont la table est a
   0x87f094 ; la vraie fonction est `sub_87eab0`. Prendre le symbole le plus
   proche comme « la fonction » est donc faux.

   `fn` ci-dessous rend la vraie fonction englobante = la cible d'appel la plus
   proche en dessous. Si elle a 0 appelant, c'est soit une entree indirecte
   (table de repartition, vtable), soit qu'on est encore dans un corps de cas :
   dans les deux cas, ne pas conclure sans verifier.

Usage :
    tools/callmap.py who  <va> [...]   qui appelle cette VA
    tools/callmap.py fn   <va> [...]   vraie fonction englobante d'une VA
    tools/callmap.py made <lo> <hi>    tous les appels emis dans [lo, hi)
    tools/callmap.py stats
"""
import struct, sys, json, os, bisect

EXE = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "boot_cache", "callmap.json")


def build():
    raw = open(EXE, 'rb').read()
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    n = struct.unpack_from('<H', raw, pe + 6)[0]
    opt = struct.unpack_from('<H', raw, pe + 20)[0]
    base = struct.unpack_from('<I', raw, pe + 24 + 28)[0]
    tva = vsz = None
    for i in range(n):
        o = pe + 24 + opt + i * 40
        if raw[o:o + 8].rstrip(b'\0') == b'.text':
            vsz, va, rsz, ro = struct.unpack_from('<IIII', raw, o + 8)
            tva, blob = base + va, raw[ro:ro + rsz]
            break
    if tva is None:
        sys.exit("pas de section .text")
    m = {}
    for i in range(len(blob) - 5):
        if blob[i] == 0xe8:
            rel = struct.unpack_from('<i', blob, i + 1)[0]
            tgt = tva + i + 5 + rel
            if tva <= tgt < tva + vsz:
                m.setdefault(tgt, []).append(tva + i)
    return m


def load():
    if os.path.exists(CACHE):
        try:
            return {int(k): v for k, v in json.load(open(CACHE)).items()}
        except Exception:
            pass
    m = build()
    try:
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        json.dump({str(k): v for k, v in m.items()}, open(CACHE, 'w'))
    except OSError:
        pass          # cache facultatif : l'outil doit marcher sans
    return m


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    m = load()
    tgts = sorted(m)
    mode = sys.argv[1]
    if mode == 'who':
        for a in sys.argv[2:]:
            t = int(a, 16)
            sites = m.get(t, [])
            print("0x%08x : %d appelant(s)" % (t, len(sites)))
            for x in sites:
                print("    0x%08x" % x)
    elif mode == 'fn':
        for a in sys.argv[2:]:
            t = int(a, 16)
            i = bisect.bisect_right(tgts, t) - 1
            if i < 0:
                print("0x%08x : aucune cible d'appel en dessous" % t)
                continue
            f = tgts[i]
            print("0x%08x  dans  sub_%06x (+0x%x)  [%d appelant(s)]"
                  % (t, f, t - f, len(m[f])))
    elif mode == 'made':
        lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
        out = [(s, t) for t, sites in m.items() for s in sites if lo <= s < hi]
        for s, t in sorted(out):
            print("    0x%08x -> sub_%06x" % (s, t))
    elif mode == 'stats':
        print("%d cibles, %d sites" % (len(m), sum(len(v) for v in m.values())))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
