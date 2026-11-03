// Interpreteur de secours (idee de recomp-kit) : le code de .text que le lift n'a pas
// (bloc manquant, saut ou appel vers une cible inconnue) s'execute sur ses octets d'origine dans X86GuestVM, au lieu
// de abort() ou d'un appel saute en silence. Ce n'est pas une garde : chaque VA nommee par [INTERP] est un trou a
// lifter (BO2_INTERP_VAS=<fichier> les y ajoute, au format de tools/lift_extra.py --list).
//
// Sortie de la VM, au debut de chaque bloc :
//  - le bloc d'avant finit par ret / ret n et vient de depiler une adresse qui etait sur la pile a l'entree : le
//    cadre est sorti, la suite appartient au code traduit de l'appelant ;
//  - un bloc traduit atteint par `call` ([ESP] = fin du bloc d'avant) : appel natif, retour dans la VM ;
//  - un bloc traduit atteint autrement (saut, chute) : la suite du MEME cadre est traduite, on la rend au natif ;
//    dans un cadre appele DEPUIS la VM (pile `calls`), on continue d'interpreter : le natif rendrait au mauvais hote.
// Imports et sentinelles : comme l'adaptateur d'exceptions, crochet de code sur leurs plages.
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" uint8_t* bo2_g_base;
extern "C" int win32_current_tid();
extern "C" bool win32_lifted_has(uint32_t va);
extern "C" void* bo2_heap_code_appel(State&,uint32_t,void*);

namespace {
using bo2::X86GuestVM;
using Action=X86GuestVM::BlockAction;
constexpr uint32_t gdt=0x7ff00000;
constexpr uint64_t addressSpace=1ull<<32;
constexpr uint32_t window=0x10000;   // octets de pile releves a l'entree, ou chercher l'adresse de retour du cadre
// Meme fin de .text que kernel32_impl.cpp (kTexteFin).
#if defined(BO2_TARGET_SPS)
constexpr uint32_t texteFin=0x9ef000u;
#elif defined(BO2_TARGET_MPS)
constexpr uint32_t texteFin=0xb09000u;
#else
constexpr uint32_t texteFin=0xb07000u;
#endif

struct Frame {
    uint32_t floor=0;               // ESP a l'entree
    std::vector<uint32_t> snap;     // [floor, floor+4*snap.size()) a l'entree
    std::vector<uint32_t> calls;    // cases de retour des `call` interpretes encore ouverts
    uint32_t exitPc=0; bool done=false, resume=false;
    bool returnSlot(uint32_t slot,uint32_t pc) const {
        if (slot<floor || (slot-floor)&3 || (slot-floor)/4>=snap.size()) return false;
        return snap[(slot-floor)/4]==pc;
    }
};
struct Vm {
    X86GuestVM vm;
    State* state=nullptr; void* memory=nullptr; Frame* frame=nullptr;
    std::unordered_set<uint32_t> imports;
    explicit Vm(State& s) {
        vm.map(0,gdt,bo2_g_base); vm.map(gdt,0x1000);
        vm.map(gdt+0x1000,addressSpace-gdt-0x1000,bo2_g_base+gdt+0x1000);
        const uint32_t teb=s.addr.fs_base.dword;
        vm.configureThread(gdt,teb,win32_mem_read32(teb+8),win32_mem_read32(teb+4));
        uint32_t iatLo=~0u,iatHi=0;
        for (unsigned i=0;iat_table[i].iat_va;++i) {
            imports.insert(iat_table[i].iat_va);
            iatLo=std::min(iatLo,uint32_t(iat_table[i].iat_va)); iatHi=std::max(iatHi,uint32_t(iat_table[i].iat_va));
        }
        // ponytail: RaiseException part au natif (pas de livraison SEH dans la VM comme l'adaptateur d'exceptions) ; a faire
        // si un [INTERP] la croise.
        vm.externalCalls([this](uint32_t a) {
            const auto hi=a>>24;
            return imports.count(a) || ((a&0xffff0000)==0xdead0000) || hi==0xe5 || hi==0xe6 || hi==0xe7;
        },[this](X86GuestVM& v,uint32_t a) {
            const uint32_t ret=v.read32(v.reg(UC_X86_REG_ESP));
            bo2::fromVm(v,*state);
            memory=bo2_heap_code_appel(*state,a,memory);
            bo2::toVm(v,*state,ret);
        },{{iatLo,iatHi},{0xdead0000u,0xdeadffffu},{0xe5000000u,0xe7ffffffu}});
        vm.blockCalls([this](uint32_t pc,uint32_t prevEnd) { return onBlock(pc,prevEnd); });
    }
    Action onBlock(uint32_t pc,uint32_t prevEnd) {
        Frame& f=*frame;
        const uint32_t esp=vm.reg(UC_X86_REG_ESP);
        if (prevEnd>=3 && esp>f.floor) {
            uint8_t b[3]; vm.read(prevEnd-3,b,3);
            const bool ret=b[2]==0xc3 && f.returnSlot(esp-4,pc);
            const bool retN=b[0]==0xc2 && f.returnSlot(esp-4-(b[1]|b[2]<<8),pc);
            if (ret || retN) { f.exitPc=pc; f.done=true; return Action::Stop; }
        }
        while (!f.calls.empty() && f.calls.back()<esp) f.calls.pop_back();   // ret (ou ret n) passe : cadre ferme
        const bool called=prevEnd && vm.read32(esp)==prevEnd;
        if (!win32_lifted_has(pc)) { if (called) f.calls.push_back(esp); return Action::Continue; }
        if (called) return Action::External;
        if (!f.calls.empty()) return Action::Continue;
        f.exitPc=pc; f.done=f.resume=true; return Action::Stop;
    }
};

std::mutex vmsMutex;
std::unordered_map<int,std::vector<std::unique_ptr<Vm>>> vms;   // par fil invite, une VM par profondeur
std::unordered_map<int,std::vector<uintptr_t>> hostSps;   // pile hote de chaque niveau ouvert
std::mutex seenMutex;
std::unordered_set<uint32_t> seen;
uint64_t entries=0;

void witness(uint32_t pc,const char* site) {
    std::lock_guard<std::mutex> lock(seenMutex);
    ++entries;
    if (seen.insert(pc).second) {
        std::fprintf(stderr,"[INTERP] 0x%08x (%s) tid=%d : octets d'origine, %zu VA distinctes\n",pc,site,win32_current_tid(),seen.size());
        static const char* const path=std::getenv("BO2_INTERP_VAS");
        if (FILE* out=path ? std::fopen(path,"a") : nullptr) { std::fprintf(out,"%x  # %s\n",pc,site); std::fclose(out); }
    }
    if (!(entries&(entries-1))) std::fprintf(stderr,"[INTERP] %llu entrees, %zu VA\n",(unsigned long long)entries,seen.size());
}
}

extern "C" bool bo2_interp_on() {
    static const bool on=[]{ const char* e=std::getenv("BO2_INTERP"); return !e || std::strcmp(e,"0")!=0; }();
    return on;
}
// Peut-on interpreter ici ? Seulement .text : une cible hors image (pointeur nul, tas) garde son ancien chemin.
extern "C" bool bo2_interp_peut(uint32_t pc) { return bo2_interp_on() && pc>=0x401000u && pc<texteFin; }

// Execute depuis pc jusqu'a la sortie du cadre. Rend le pc ou reprendre ; *traduite = vrai si c'est un bloc traduit
// du meme cadre (a lancer en natif), faux si le cadre est sorti par ret (pc = adresse de retour). Echec = abort.
extern "C" uint32_t bo2_interp(State& s,uint32_t pc,void** mem,const char* site,bool* traduite) {
    witness(pc,site);
    const int tid=win32_current_tid();
    Vm* v=nullptr; unsigned depth=0;
    try {
        {
            std::lock_guard<std::mutex> lock(vmsMutex);
            // Un _longjmp (bo2_reprise_non_locale) saute par-dessus bo2_interp : un niveau ouvert plus profond que
            // nous sur la pile hote est mort, sa VM reste « running ». On la jette.
            const uintptr_t here=reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
            auto& sps=hostSps[tid]; auto& stack=vms[tid];
            while (!sps.empty() && sps.back()<=here) { stack[sps.size()-1].reset(); sps.pop_back(); }
            depth=unsigned(sps.size()); sps.push_back(here);
            if (stack.size()<=depth) stack.resize(depth+1);
            if (!stack[depth]) stack[depth]=std::make_unique<Vm>(s);
            v=stack[depth].get();
        }
        Frame f; f.floor=s.gpr.rsp.dword;
        const uint32_t high=win32_mem_read32(s.addr.fs_base.dword+4);   // StackBase du TEB
        const uint32_t bytes=high>f.floor ? std::min(window,high-f.floor) : 0;
        f.snap.resize(bytes/4);
        std::memcpy(f.snap.data(),bo2_g_base+f.floor,f.snap.size()*4);   // carte plate
        Frame* outer=v->frame; State* outerState=v->state;
        v->frame=&f; v->state=&s; v->memory=*mem;
        bo2::toVm(v->vm,s,pc);
        const auto r=v->vm.run(pc,0,{~0ull,0xffffffffu});
        bo2::fromVm(v->vm,s);
        *mem=v->memory; v->frame=outer; v->state=outerState;
        { std::lock_guard<std::mutex> lock(vmsMutex); hostSps[tid].pop_back(); }
        if (!r.returned || !f.done) {
            std::fprintf(stderr,"[INTERP] 0x%08x (%s) tid=%d profondeur=%u : echec a pc=%08x apres %llu blocs : %s\n",pc,site,tid,
                         depth,r.pc,(unsigned long long)r.blocks,r.returned ? "arret hors sortie de cadre" : r.error.c_str());
            std::abort();
        }
        *traduite=f.resume;
        return f.exitPc;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"[INTERP] 0x%08x (%s) tid=%d : %s\n",pc,site,tid,e.what());
        std::abort();
    }
}
