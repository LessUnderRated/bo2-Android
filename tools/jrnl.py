#!/usr/bin/env python3
"""Read a BO2 replay journal offline — no run required.

    tools/jrnl.py <journal>              stats: records, imports, writes, coverage
    tools/jrnl.py <a> <b>                DIFF two journals: the first record that
                                         differs, which is the exact ordinal at
                                         which two runs stopped being the same run

⚠️ A journal without its end marker was CUT OFF (the run was killed or crashed).
That is reported, because "the recording is short" and "the replay went further"
are opposite findings and a reader that conflates them is useless.
"""
import struct, sys, collections, os

MAGIC = b"BO2JRNL1"
HDR = struct.Struct("<8sII")
REC = struct.Struct("<IIHHIIII")     # seq iat tid nwrites eax edx esp_in esp_out
WR  = struct.Struct("<III")
END = 0xffffffff

PSEUDO = {0: "<input: window message>", 1: "<audio: CoreAudio credit>"}

def iat_names():
    """Name IAT slots from the generated header, so the tool needs no run."""
    names = {}
    h = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "runtime", "win32",
                     "iat_table_%s.h" % os.environ.get("BO2_TARGET", "sps"))
    try:
        import re
        for m in re.finditer(r'\{\s*0x([0-9a-fA-F]+),\s*"([^"]+)",\s*"([^"]+)"\s*\}', open(h).read()):
            names[int(m.group(1), 16)] = m.group(3)
    except OSError:
        pass
    names.update(PSEUDO)   # the two non-import boundaries share the file
    return names

def read(path):
    b = open(path, "rb").read()
    magic, ver, flags = HDR.unpack_from(b, 0)
    if magic != MAGIC:
        sys.exit(f"{path}: not a journal (magic {magic!r})")
    off = HDR.size
    recs, complete = [], False
    while off + REC.size <= len(b):
        seq, iat, tid, nw, eax, edx, esp_in, esp_out = REC.unpack_from(b, off)
        off += REC.size
        if seq == END and iat == 0 and nw == 0:
            complete = True
            break
        if tid & 0x8000:            # in flight when the recording ended
            recs.append((seq, iat, tid, eax, edx, esp_in, esp_out, []))
            continue
        writes = []
        for _ in range(nw):
            if off + WR.size > len(b):
                break
            writes.append(WR.unpack_from(b, off)); off += WR.size
        recs.append((seq, iat, tid, eax, edx, esp_in, esp_out, writes))
    return recs, complete, ver, flags

def stats(path):
    recs, complete, ver, flags = read(path)
    nm = iat_names()
    print(f"{path}")
    print(f"  version {ver}  flags 0x{flags:x}{'  (every import)' if flags & 1 else ''}")
    print(f"  records          {len(recs)}")
    print(f"  end marker       {'yes' if complete else 'NO — RECORDING WAS CUT OFF'}")
    if not recs:
        print("  (empty — the journal was armed but nothing was recorded)")
        return
    print(f"  import ordinals  {recs[0][0]} .. {recs[-1][0]}")
    print(f"  writes captured  {sum(len(r[7]) for r in recs)}")
    inflight = sum(1 for r in recs if r[2] & 0x8000)
    print(f"  in flight at end {inflight}   (shims entered and never returned before "
          f"the recording stopped — not errors)")
    print(f"  file size        {os.path.getsize(path)/1048576:.1f} MiB")
    aux = collections.Counter(r[1] for r in recs if r[1] in PSEUDO)
    if aux:
        print("  non-import boundaries:")
        for k, v in sorted(aux.items()):
            print(f"     {v:8d}  {PSEUDO[k]}")
    c = collections.Counter(nm.get(r[1], f"0x{r[1]:x}") for r in recs)
    t = collections.Counter(r[2] & 0x7fff for r in recs)
    print(f"  guest threads    {dict(sorted(t.items()))}")
    print("  top imports:")
    for k, v in c.most_common(15):
        print(f"     {v:8d}  {k}")

def diff(pa, pb):
    ra, ca, _, _ = read(pa)
    rb, cb, _, _ = read(pb)
    nm = iat_names()
    print(f"A {pa}: {len(ra)} records{'' if ca else '  (CUT OFF)'}")
    print(f"B {pb}: {len(rb)} records{'' if cb else '  (CUT OFF)'}")
    n = min(len(ra), len(rb))
    for i in range(n):
        # identity of a record = (ordinal, slot, thread, result). Values differing
        # is the entropy; ordinals differing is a different PROGRAM.
        if ra[i][:5] != rb[i][:5]:
            a, b = ra[i], rb[i]
            print(f"\nfirst differing record: index {i}")
            for tag, r in (("A", a), ("B", b)):
                print(f"  {tag}  seq={r[0]:<9} {nm.get(r[1], hex(r[1])):<28} "
                      f"tid={r[2]} eax=0x{r[3]:08x} edx=0x{r[4]:08x}")
            same_ordinal = a[0] == b[0] and a[1] == b[1] and a[2] == b[2]
            print("  -> same ordinal and slot, DIFFERENT VALUE: this is entropy the "
                  "journal captures (expected)" if same_ordinal else
                  "  -> DIFFERENT ORDINAL OR SLOT: the two runs executed different "
                  "code by here")
            print(f"  identical prefix: {i} records "
                  f"(ordinals {ra[0][0]}..{ra[i-1][0] if i else '-'})")
            return
    print(f"\nno differing record in the common prefix of {n}; "
          f"lengths {len(ra)} vs {len(rb)}")

if __name__ == "__main__":
    if len(sys.argv) == 2:   stats(sys.argv[1])
    elif len(sys.argv) == 3: diff(sys.argv[1], sys.argv[2])
    else: sys.exit(__doc__)
