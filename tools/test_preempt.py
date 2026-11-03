#!/usr/bin/env python3
"""Compile the production import hook with a counted scheduler, exercise exclusions."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / 'runtime/win32/kernel32_impl.cpp').read_text()
start = src.index('extern "C" void win32_thread_import_tick() {')
end = src.index('\n}\n', start) + 3
hook = src[start:end]
prefix = '''#include <cstdint>
#include <cstdlib>
#include <cstdio>
static unsigned long long g_imp_total=0, g_imp_under_lock=0;
uint64_t g_setorigin_completed=0;
static int g_cur=0, g_cs_depth[64]={}, g_in_shim_callback=0, yields=0;
static const char* bo2_getenv(const char* s) { return std::getenv(s); }
static void coop_yield() { ++yields; }
'''
main = '''int main() {
  const bool delayed=std::getenv("BO2_PREEMPT_AFTER_ORIGIN") != nullptr;
  if(delayed) {
    for(int i=0;i<8;i++) win32_thread_import_tick();
    if(yields) return 6;
    g_setorigin_completed=2;
  }
  g_in_shim_callback=1;
  for(int i=0;i<8;i++) win32_thread_import_tick();
  if(yields) return 1;
  g_in_shim_callback=0; g_cs_depth[0]=1;
  for(int i=0;i<8;i++) win32_thread_import_tick();
  if(yields) return 2;
  g_cs_depth[0]=0;
  win32_thread_import_tick();
  if(yields) return 3;
  win32_thread_import_tick();
  const bool enabled=std::getenv("BO2_PREEMPT") != nullptr;
  if(yields != (enabled ? 1 : 0)) return 4;
  if(g_imp_total != (delayed ? 26 : 18) || g_imp_under_lock != 8) return 5;
  std::puts("callback, lock, resumed timeslice and control counters: pass");
}
'''
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / 'probe.cpp'
    exe = Path(temp) / 'probe'
    cpp.write_text(prefix + hook + main)
    subprocess.run(['/opt/homebrew/opt/llvm@17/bin/clang++', '-O2', '-std=c++17',
                    str(cpp), '-o', str(exe)], check=True)
    env = dict(os.environ)
    env.pop('BO2_PREEMPT', None)
    subprocess.run([str(exe)], env=env, check=True)
    env['BO2_PREEMPT'] = '2'
    subprocess.run([str(exe)], env=env, check=True)
    env['BO2_PREEMPT_AFTER_ORIGIN'] = '2'
    subprocess.run([str(exe)], env=env, check=True)
