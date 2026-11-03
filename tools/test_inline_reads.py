#!/usr/bin/env python3
"""Compile the production IR transform and check boundary values and slow paths.

Uses a sparse 4 GiB mapping, touching only test addresses. No game/lifter needed.
"""
import ast
import os
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CLANG = '/opt/homebrew/opt/llvm@17/bin/clang++'
tree = ast.parse((ROOT / 'tools/boot.py').read_text())
nodes = [n for n in tree.body if
         isinstance(n, ast.FunctionDef) and n.name == '_inline_guest_reads' or
         isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and
                                           t.id == '_READ_KINDS' for t in n.targets)]
scope = {'re': re, 'os': os}
exec(compile(ast.Module(body=nodes, type_ignores=[]), 'boot.py', 'exec'), scope)
ir = ''
for suf, ty, attr, _ in scope['_READ_KINDS']:
    ir += f'''declare {attr}{ty} @__remill_read_memory_{suf}(ptr, i32)
define {attr}{ty} @test_{suf}(i32 %a) {{
  %v = call {attr}{ty} @__remill_read_memory_{suf}(ptr null, i32 %a)
  ret {ty} %v
}}
'''
ir = scope['_inline_guest_reads'](ir)
harness = r'''
#include <sys/mman.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
extern "C" {
uint8_t *bo2_g_base;
int g_park_pending;
uint8_t test_8(uint32_t);
uint16_t test_16(uint32_t);
uint32_t test_32(uint32_t);
float test_f32(uint32_t);
static unsigned calls;
static uint32_t reference(uint32_t a, unsigned n) {
  uint32_t v=0;
  for(unsigned i=0;i<n;i++) v |= uint32_t(bo2_g_base[uint32_t(a+i)]) << (8*i);
  return v;
}
uint8_t __remill_read_memory_8_slow(void*,uint32_t a) { ++calls; return reference(a,1); }
uint16_t __remill_read_memory_16_slow(void*,uint32_t a) { ++calls; return reference(a,2); }
uint32_t __remill_read_memory_32_slow(void*,uint32_t a) { ++calls; return reference(a,4); }
float __remill_read_memory_f32_slow(void*,uint32_t a) {
  ++calls; uint32_t v=reference(a,4); float f; memcpy(&f,&v,4); return f;
}
}
int main() {
  bo2_g_base=(uint8_t*)mmap(nullptr,1ULL<<32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
  if (bo2_g_base==MAP_FAILED) return 2;
  unsigned checked=0;
  const uint32_t bases[]={0,0x10000,0x80000000,0xffff0000};
  for(uint32_t base:bases) for(unsigned off: {0u,1u,65531u,65532u,65533u,65534u,65535u}) {
    uint32_t a=base+off;
    for(unsigned i=0;i<4;i++) bo2_g_base[uint32_t(a+i)]=uint8_t(0x91+i*23);
    for(int park=0;park<2;park++) for(unsigned kind=0;kind<4;kind++) {
      g_park_pending=park; calls=0; unsigned n=kind==0?1:kind==1?2:4;
      uint32_t got;
      if(kind==0) got=test_8(a);
      else if(kind==1) got=test_16(a);
      else if(kind==2) got=test_32(a);
      else {float f=test_f32(a); memcpy(&got,&f,4);}
      unsigned expected_calls=park || (a&65535)>65536-n;
      if(got!=reference(a,n) || calls!=expected_calls) {
        fprintf(stderr,"FAIL addr=%x kind=%u park=%d got=%x expected=%x calls=%u/%u\n",
                a,kind,park,got,reference(a,n),calls,expected_calls); return 1;
      }
      ++checked;
    }
  }
  munmap(bo2_g_base,1ULL<<32);
  printf("%u boundary/value/slow-path checks passed\n",checked);
}
'''.replace('#include <cstdlib>', '#include <cstdlib>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='bo2-read-test-') as d:
    d = pathlib.Path(d)
    (d/'reads.ll').write_text(ir)
    (d/'check.cpp').write_text(harness)
    for opt in ('-O0', '-O2'):
        subprocess.run([CLANG, opt, '-Wno-override-module', str(d/'reads.ll'),
                        str(d/'check.cpp'), '-o', str(d/'check')], check=True)
        print(opt, flush=True)
        subprocess.run([str(d/'check')], check=True)


# ── the hazard the value tests cannot see: HOISTING ───────────────────────────
#
# A call is an optimisation barrier; an inlined load is not. In a guest loop
# containing no call -- the shape this port is documented to have, where a thread
# spins inside ONE lifted block -- LLVM may hoist the load into the preheader.
# The worse of the two loads is g_park_pending: it is polled THROUGH this
# intrinsic precisely because such a loop has no other poll point, so hoisting it
# means a destroyed thread can never retire.
#
# Signature of the hazard, as observed: the load lands in the entry/preheader
# region, before the first `br label`. Asserted for the shipping mode; merely
# reported for the non-volatile mode, so that a future LLVM being less eager
# tells us the test lost its teeth instead of failing the build.
SPIN = """declare i32 @__remill_read_memory_32(ptr, i32)
define i32 @spin(ptr %mem, i32 %a) {
entry:
  br label %loop
loop:
  %v = call i32 @__remill_read_memory_32(ptr %mem, i32 %a)
  %done = icmp eq i32 %v, 7
  br i1 %done, label %out, label %loop
out:
  ret i32 %v
}
"""
LLVM = pathlib.Path(CLANG).parent

def _preheader_hoists_park(mode):
    os.environ['BO2_INLINE_READS'] = mode
    with tempfile.TemporaryDirectory(prefix='bo2-hoist-') as d:
        d = pathlib.Path(d)
        (d / 'spin.ll').write_text(scope['_inline_guest_reads'](SPIN))
        subprocess.run([str(LLVM / 'opt'), '-O2', '-S', '-o', str(d / 'o2.ll'),
                        str(d / 'spin.ll')], check=True)
        body = (d / 'o2.ll').read_text()
        body = body[body.index('define i32 @spin'):]
        body = body[:body.index('\n}')]
        return '@g_park_pending' in body[:body.index('br label')]

if _preheader_hoists_park('1'):
    raise SystemExit('FAIL: the park flag was hoisted into the preheader even with '
                     'volatile -- a guest spin loop would poll it once, ever')
print('hoisting: park flag stays in the loop under BO2_INLINE_READS=1')
if not _preheader_hoists_park('2'):
    print('NOTE: BO2_INLINE_READS=2 no longer hoists either; this LLVM is less '
          'eager, so the check above no longer proves much. Re-derive the hazard.')
else:
    print('hoisting: confirmed the non-volatile mode DOES hoist it (control)')
