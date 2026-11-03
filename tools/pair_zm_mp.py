"""tools/pair_zm_mp.py <exe_zm> <exe_mp> <liste_va_zm> <sortie> -> combien de fonctions ZM ont une jumelle unique dans MP.
Signature = 16 premières instructions, adresses et immédiats masqués (mnémonique + registres seulement)."""
import sys, struct, re, collections
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_parser import PEFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
cs = Cs(CS_ARCH_X86, CS_MODE_32)
HEX = re.compile(r'0x[0-9a-f]+|\b\d+\b')

def image(path):
    pe = PEFile(path).parse(); mem = {}
    for s in pe.sections:
        off = pe.rva_to_offset(s.virtual_address)
        mem[pe.image_base + s.virtual_address] = pe._raw[off:off + s.size_of_raw_data]
    return mem

def lire(mem, va, n=160):
    for b, d in mem.items():
        if b <= va < b + len(d): return d[va - b: va - b + n]
    return b''

def signature(mem, va):
    out = []
    for i in cs.disasm(lire(mem, va), va):
        out.append(i.mnemonic + ' ' + HEX.sub('#', i.op_str))
        if len(out) == 16 or i.mnemonic in ('ret', 'jmp'): break
    return '|'.join(out) if len(out) >= 4 else None

def cibles_call(mem):
    t = set()
    for b, d in mem.items():
        for m in re.finditer(rb'\xe8', d):
            o = m.start()
            if o + 5 > len(d): continue
            c = b + o + 5 + struct.unpack_from('<i', d, o + 1)[0]
            t.add(c)
        for o in range(0, len(d) - 3, 4):                 # pointeurs de code dans les données (vtables, tables)
            t.add(struct.unpack_from('<I', d, o)[0])
    lo = min(mem); hi = max(b + len(d) for b, d in mem.items())
    return {c for c in t if lo <= c < hi}

zm, mp = image(sys.argv[1]), image(sys.argv[2])
vas = [int(l.split(':')[0], 16) for l in open(sys.argv[3]) if l.strip()]
idx = collections.defaultdict(list)
for c in cibles_call(mp):
    s = signature(mp, c)
    if s: idx[s].append(c)
uni = multi = rien = court = 0
paires = {}
for va in vas:
    s = signature(zm, va)
    if not s: court += 1; continue
    h = idx.get(s, [])
    if len(h) == 1: uni += 1; paires[va] = h[0]
    elif h: multi += 1
    else: rien += 1
print(f"ZM={len(vas)} jumelle_unique={uni} ambiguë={multi} absente={rien} trop_courte={court}")
with open(sys.argv[4], 'w') as f:
    for a, b in sorted(paires.items()): f.write(f"{a:x} {b:x}\n")
