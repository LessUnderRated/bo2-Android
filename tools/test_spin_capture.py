#!/usr/bin/env python3
"""Exercise the production SIGPROF handler on an mmap-backed coroutine stack."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root/'runtime/win32/kernel32_impl.cpp').read_text()
start = source.index('static std::atomic<void*> g_spin_pc')
end = source.index('// ── 🎯 BO2_SPIN_HIST', start)
handler = source[start:end]
harness = r'''
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <ucontext.h>
#include <signal.h>
#include <sys/mman.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
HANDLER
static ucontext_t caller, coroutine;
static void body() {
    for (unsigned i=0;i<10000;i++) {
        g_spin_n.store(-1);
        raise(SIGPROF);
        if (g_spin_n.load()!=1 || !g_spin_pc.load()) abort();
    }
}
int main() {
    struct sigaction sa{};
    sa.sa_sigaction=spin_sigprof;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags=SA_SIGINFO|SA_RESTART;
    if(sigaction(SIGPROF,&sa,nullptr)) return 2;
    void* stack=mmap(nullptr,1<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(stack==MAP_FAILED || getcontext(&coroutine)) return 2;
    coroutine.uc_stack.ss_sp=stack;
    coroutine.uc_stack.ss_size=1<<20;
    coroutine.uc_link=&caller;
    makecontext(&coroutine,body,0);
    if(swapcontext(&caller,&coroutine)) return 2;
    munmap(stack,1<<20);
    puts("10000 SIGPROF captures on a coroutine stack passed");
}
'''.replace('HANDLER', handler)
with tempfile.TemporaryDirectory(prefix='bo2-spin-test-') as d:
    d = pathlib.Path(d)
    (d/'test.cpp').write_text(harness)
    subprocess.run(['/opt/homebrew/opt/llvm@17/bin/clang++', '-std=c++17', '-O2',
                    '-Wno-deprecated-declarations', str(d/'test.cpp'),
                    '-o', str(d/'test')], check=True)
    subprocess.run([str(d/'test')], check=True, timeout=20)
