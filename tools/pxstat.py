#!/usr/bin/env python3
"""Whole-image statistics for a screenshot. USE THIS, NOT A SAMPLED BAND.

⚠️ WHY THIS FILE EXISTS, and it cost most of a night. I measured the frontend by
decoding a few pixels out of the middle of the picture — rows 30..300 of a
528-row image, the middle third horizontally — got (40,41,44) every time, and
built an entire investigation on "the menu room renders as a flat dark constant".

The room renders CORRECTLY. That band is the dark upper wall. The full image has
mean 150.8 and a spread across every brightness bucket: lights, posters, the 3D
scene, and readable MULTIPLAYER / CAMPAIGN / ZOMBIES / QUIT.

docs/GUIDE.md §9 already carries the rule I broke: "Measure the percept. The
number saying yes and the picture saying no means the picture wins." I never
looked at the picture until the end. LOOK AT IT FIRST, then measure the whole
frame, and only then reason.

    tools/pxstat.py 'docs/refs/*.png'
"""
import zlib, struct, collections, os, glob, sys
def read_png(path):
    d=open(path,'rb').read(); pos=8; idat=b''
    while pos<len(d):
        ln=struct.unpack_from('>I',d,pos)[0]; typ=d[pos+4:pos+8]
        data=d[pos+8:pos+8+ln]; pos+=12+ln
        if typ==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB',data[:10])
        elif typ==b'IDAT': idat+=data
        elif typ==b'IEND': break
    raw=zlib.decompress(idat); ch={0:1,2:3,3:1,4:2,6:4}[ct]; stride=w*ch
    out=bytearray(); prev=bytearray(stride); i=0
    for y in range(h):
        f=raw[i]; i+=1
        line=bytearray(raw[i:i+stride]); i+=stride
        if f==1:
            for x in range(ch,stride): line[x]=(line[x]+line[x-ch])&255
        elif f==2:
            for x in range(stride): line[x]=(line[x]+prev[x])&255
        elif f==3:
            for x in range(stride):
                a=line[x-ch] if x>=ch else 0
                line[x]=(line[x]+((a+prev[x])>>1))&255
        elif f==4:
            for x in range(stride):
                a=line[x-ch] if x>=ch else 0
                b=prev[x]; c=prev[x-ch] if x>=ch else 0
                p=a+b-c; pa,pb,pc=abs(p-a),abs(p-b),abs(p-c)
                pr=a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[x]=(line[x]+pr)&255
        out+=line; prev=line
    return w,h,ch,bytes(out)

# Guarded so read_png can be IMPORTED (tools/shotdiff.py reuses it) instead of
# copied — a second decoder would be a second thing to get wrong.
if __name__ == "__main__":
    for p in sorted(glob.glob(sys.argv[1])):
        w,h,ch,px=read_png(p)
        # skip the title bar (top ~26px)
        tot=0; n=0; hist=[0]*8; mn=255; mx=0
        for y in range(30,h,3):
            for x in range(0,w,3):
                o=(y*w+x)*ch
                v=(px[o]+px[o+1]+px[o+2])//3
                tot+=v; n+=1; hist[min(7,v>>5)]+=1
                mn=min(mn,v); mx=max(mx,v)
        print(f"{os.path.basename(p):44s} mean={tot/max(n,1):6.1f} min={mn:3d} max={mx:3d} "
              f"buckets={['%d%%'%(100*c//max(n,1)) for c in hist]}")
