// Original x86 fallback for exception delivery (vectored handlers, RaiseException): Unicorn runs the
// guest's own handler bytes. Linked into the three Steam targets.
#include "win32_shim.h"
#include "x86_guest_vm.h"
#include "x86_state_transfer.h"
#if defined(BO2_TARGET_SPS)
#include "iat_table_sps.h"
#elif defined(BO2_TARGET_MPS)
#include "iat_table_mps.h"
#else
#include "iat_table_zms.h"
#endif
#include <unicorn/x86.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

extern "C" uint8_t* bo2_g_base;
extern "C" int win32_current_tid();
extern "C" void* bo2_heap_code_appel(State&,uint32_t,void*);

namespace {
using bo2::X86GuestVM;
using bo2::toVm;
using bo2::fromVm;
constexpr uint32_t gdt=0x7ff00000;
constexpr uint64_t addressSpace=1ull<<32;
struct Veh { uint32_t handle,address; };
std::mutex registryMutex;
std::vector<Veh> registry;
uint64_t registryVersion=0;
uint32_t nextHandle=0x7ec10000;


bool pureHash(uint32_t a) {
    (void)a; return false;
}
void* callNative(State& s,uint32_t a,void* mem) {
    return bo2_heap_code_appel(s,a,mem);
}
struct ThreadVm {
    X86GuestVM vm;
    State* activeState=nullptr;
    void* memory=nullptr;
    uint64_t version=~0ull;
    std::vector<uint32_t> handles;
    std::unordered_map<uint32_t,const char*> imports;
    void syncVeh() {
        std::lock_guard<std::mutex> lock(registryMutex);
        if (version==registryVersion) return;
        for (auto h:handles) vm.removeVeh(h);
        handles.clear();
        for (auto h:registry) handles.push_back(vm.addVeh(false,h.address));
        version=registryVersion;
    }
    explicit ThreadVm(State& s) {
        vm.map(0,gdt,bo2_g_base); vm.map(gdt,0x1000);
        vm.map(gdt+0x1000,addressSpace-gdt-0x1000,bo2_g_base+gdt+0x1000);
        const uint32_t teb=s.addr.fs_base.dword;
        vm.configureThread(gdt,teb,win32_mem_read32(teb+8),win32_mem_read32(teb+4));
        uint32_t iatLo=~0u,iatHi=0;
        for (unsigned i=0;iat_table[i].iat_va;++i) {
            imports[iat_table[i].iat_va]=iat_table[i].name;
            iatLo=std::min(iatLo,uint32_t(iat_table[i].iat_va)); iatHi=std::max(iatHi,uint32_t(iat_table[i].iat_va));
        }
        vm.externalCalls([this](uint32_t a) {
            const auto hi=a>>24;
            return imports.count(a) || ((a&0xffff0000)==0xdead0000) || hi==0xe5 || hi==0xe6 || hi==0xe7 || pureHash(a);
        },[this](X86GuestVM& v,uint32_t a) {
            auto it=imports.find(a);
            if (it!=imports.end() && std::strcmp(it->second,"RaiseException")==0) {
                uint32_t esp=v.reg(UC_X86_REG_ESP);
                v.raiseException(v.read32(esp+4),v.read32(esp+8),v.read32(esp+12),v.read32(esp+16));
                return;
            }
            uint32_t ret=v.read32(v.reg(UC_X86_REG_ESP));
            fromVm(v,*activeState);
            memory=callNative(*activeState,a,memory);
            toVm(v,*activeState,ret);
            syncVeh();
        },{{iatLo,iatHi},{0xdead0000u,0xdeadffffu},{0xe5000000u,0xe7ffffffu}});   // where the predicate can be true
    }
};
std::vector<uint32_t>& entries() { static std::vector<uint32_t> v; return v; }   // every va run from original bytes
std::mutex threadsMutex;
std::unordered_map<int,std::unique_ptr<ThreadVm>> threads;
ThreadVm& threadVm(State& state) {
    std::lock_guard<std::mutex> lock(threadsMutex);
    auto& ptr=threads[win32_current_tid()];
    if (!ptr) ptr=std::make_unique<ThreadVm>(state);
    return *ptr;
}
void* execute(State& s,uint32_t pc,void* mem) {
    try {
        auto& t=threadVm(s);
        if (t.activeState) throw std::runtime_error("nested original-byte adapter call");
        t.syncVeh(); t.activeState=&s; t.memory=mem;
        const uint32_t ret=win32_mem_read32(s.gpr.rsp.dword);
        toVm(t.vm,s,pc);
        const auto t0=std::chrono::steady_clock::now();
        auto result=t.vm.run(pc,ret,{200000000,60000});
        const long long us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-t0).count();
        fromVm(t.vm,s); t.activeState=nullptr;
        const auto& dr=t.vm.debugRegisters();
        // us= : temps mur de la VM, appels externes compris ; vidages= du cache de traduction pendant cette
        // execution (code invite change hors VM), pages= de code suivies, acces= vus par le crochet memoire (15c)
        std::fprintf(stderr,"[X86-EXCEPT] tid=%d entry=%08x pc=%08x blocs=%llu exceptions=%llu %s dr=%08x,%08x,%08x,%08x dr6=%08x dr7=%08x us=%lld "
                     "vidages=%llu pages=%llu acces=%llu externes=%llu\n",
                     win32_current_tid(),pc,result.pc,(unsigned long long)result.blocks,
                     (unsigned long long)result.exceptions,result.returned?"returned":result.error.c_str(),
                     dr[0],dr[1],dr[2],dr[3],dr[4],dr[5],us,(unsigned long long)result.flushes,
                     (unsigned long long)result.codePages,(unsigned long long)result.memoryCalls,
                     (unsigned long long)result.externals);
        if (!result.returned) std::abort();
        // Z-NATIF-09m: an execution breakpoint still armed here fires only if the code at that address runs from
        // original bytes. Not an adapter entry -> translated code will run through it untrapped; name it.
        for (unsigned i=0;i<4;++i)
            if ((dr[5]&(1u<<(2*i))) && !((dr[5]>>(16+4*i))&3) && !std::count(entries().begin(),entries().end(),dr[i]))
                std::fprintf(stderr,"[X86-ARME] tid=%d entry=%08x : point d'execution arme en %08x (Dr%u, dr7=%08x), "
                             "hors adaptateur : le code traduit ne le prendra pas\n",win32_current_tid(),pc,dr[i],i,dr[5]);
        return t.memory;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"[X86-EXCEPT] tid=%d entry=%08x adapter failure: %s\n",win32_current_tid(),pc,e.what());
        std::abort();
    }
}
}
extern "C" uint32_t bo2_add_veh(uint32_t first,uint32_t handler) {
    if (!handler) return 0;
    std::lock_guard<std::mutex> lock(registryMutex);
    uint32_t handle=nextHandle++;
    if (first) registry.insert(registry.begin(),{handle,handler}); else registry.push_back({handle,handler});
    ++registryVersion; return handle;
}
extern "C" uint32_t bo2_remove_veh(uint32_t handle) {
    std::lock_guard<std::mutex> lock(registryMutex);
    auto it=std::find_if(registry.begin(),registry.end(),[&](auto v){return v.handle==handle;});
    if (it==registry.end()) return 0;
    registry.erase(it); ++registryVersion; return 1;
}
// Z-NATIF-08o: RaiseException called by TRANSLATED code gets the delivery Windows gives it (vectored handlers,
// then the fs:[0] chain), by the dispatcher that already serves VM code: the entry is the import address
// itself, which the VM recognises as an external call and turns into a pending exception.
extern "C" void bo2_raise_exception(State* s) {
    static const uint32_t entry=[]{
        for (unsigned i=0;iat_table[i].iat_va;++i)
            if (!std::strcmp(iat_table[i].name,"RaiseException")) return uint32_t(iat_table[i].iat_va);
        std::abort();
    }();
    execute(*s,entry,nullptr);
}
