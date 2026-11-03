#!/usr/bin/env python3
"""Points d'entree reels (cibles de `call rel32`) dans le bloc CRT 0x95e000-0x960000,
avec leur nombre de sites d'appel. Sert a trouver les SOEURS non interposees de
memcpy/memmove, qui partagent le meme repartiteur par table de saut."""
import struct
EXE="/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"
def sections(raw):
    pe=struct.unpack_from('<I',raw,0x3c)[0]; n=struct.unpack_from('<H',raw,pe+6)[0]
    opt=struct.unpack_from('<H',raw,pe+20)[0]; base=struct.unpack_from('<I',raw,pe+24+28)[0]
    o=[]
    for i in range(n):
        q=pe+24+opt+i*40
        nm=raw[q:q+8].rstrip(b'\0').decode(errors='replace')
        vsz,va,rsz,ro=struct.unpack_from('<IIII',raw,q+8)
        o.append((nm,base+va,vsz,ro,rsz))
    return o
raw=open(EXE,'rb').read(); s=[x for x in sections(raw) if x[0]=='.text'][0]
_,tva,tvsz,tro,trsz=s; n=min(tvsz,trsz); blob=raw[tro:tro+n]
LO,HI=0x95e000,0x960000
cnt={}
for i in range(n-5):
    if blob[i]==0xe8:
        rel=struct.unpack_from('<i',blob,i+1)[0]
        t=tva+i+5+rel
        if LO<=t<HI: cnt[t]=cnt.get(t,0)+1
print("entrees CRT appelees directement (VA, sites d'appel) :")
for t in sorted(cnt): print("  0x%06x  %6d sites" % (t,cnt[t]))
