#!/usr/bin/env python3
"""Compile production string/key decoding against controlled engine-table fixtures."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'runtime/win32/win32_runtime.cpp').read_text()
start=s.index('static bool sl_read_record(')
end=s.index('uint64_t g_cast_total',start)
code=s[start:end]
harness=r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <cstdlib>
static std::unordered_map<uint32_t,uint8_t> ram;
uint8_t win32_mem_read8(uint32_t a) {return ram[a];}
uint32_t win32_mem_read32(uint32_t a) {uint32_t v=0;for(int i=0;i<4;i++)v|=uint32_t(ram[a+i])<<(8*i);return v;}
static void put32(uint32_t a,uint32_t v){for(int i=0;i<4;i++)ram[a+i]=v>>(8*i);}
CODE
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}++checks;} while(0)
int main(){
 unsigned checks=0; constexpr uint32_t pool=0x200000, buckets=0x400000;
 put32(0x2d43da4,pool);put32(0xbf0f00,buckets);
 CHECK(!sl_pool_layout_ok()); // empty control must fail
 for(unsigned id=1;id<=64;id++){
   std::string name="field_"+std::to_string(id);
   const unsigned size=name.size()+1; const uint32_t rec=pool+id*24;
   ram[rec+3]=size;
   uint32_t h=0;
   for(unsigned j=0;j<size;j++){ram[rec+4+j]=name[j];h=h*31u+uint8_t(name[j]);}
   const uint32_t bucket=h%0xbfff+1;
   CHECK(win32_mem_read32(buckets+bucket*8)==0); // fixtures have distinct buckets
   put32(buckets+bucket*8,0x20000|bucket);put32(buckets+bucket*8+4,id);
   if(id==63) CHECK(!sl_pool_layout_ok());
 }
 CHECK(sl_pool_layout_ok());CHECK(g_sl_control_valid==64);
 CHECK(std::string(win32_sl_name(1))=="field_1");
 CHECK(win32_sl_name(0x112eb)==nullptr);
 char label[64];
 CHECK(std::string(sl_key_label(0x112eb,label,sizeof label))=="object-key:0x12eb");
 CHECK(std::string(sl_key_label(0x112e9,label,sizeof label))=="object-key:0x12e9");
 CHECK(std::string(sl_key_label(0x801176,label,sizeof label))=="integer-key:4470");
 CHECK(std::string(sl_key_label(0x15000,label,sizeof label))=="instance-dependent-key");
 ram[pool+24+4]='x';CHECK(win32_sl_name(1)==nullptr); // printable but wrong hash
 ram[pool+48+3]--;CHECK(win32_sl_name(2)==nullptr); // old off-by-one length
 put32(0x2d43da4,pool+4);CHECK(!sl_pool_layout_ok()); // wrong layout cannot pass
 printf("%u string/key/control checks passed\n",checks);
}
'''.replace('CODE',code)
with tempfile.TemporaryDirectory(prefix='bo2-sl-test-') as d:
 p=Path(d);(p/'test.cpp').write_text(harness)
 subprocess.run(['/opt/homebrew/opt/llvm@17/bin/clang++','-std=c++17','-O2',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
