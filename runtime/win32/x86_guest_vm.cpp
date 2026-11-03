#include "x86_guest_vm.h"
#include <unicorn/x86.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bo2 {
namespace {
void check(uc_err e) { if (e != UC_ERR_OK) throw std::runtime_error(uc_strerror(e)); }
constexpr uint32_t singleStep = 0x80000004, breakpoint = 0x80000003;
constexpr uint32_t contextSize = 0x2d0;
struct Field { int id; uint32_t offset, group; };
constexpr Field fields[] = {
    {UC_X86_REG_GS,0x8c,4}, {UC_X86_REG_FS,0x90,4}, {UC_X86_REG_ES,0x94,4}, {UC_X86_REG_DS,0x98,4},
    {UC_X86_REG_EDI,0x9c,2}, {UC_X86_REG_ESI,0xa0,2}, {UC_X86_REG_EBX,0xa4,2},
    {UC_X86_REG_EDX,0xa8,2}, {UC_X86_REG_ECX,0xac,2}, {UC_X86_REG_EAX,0xb0,2},
    {UC_X86_REG_EBP,0xb4,1}, {UC_X86_REG_EIP,0xb8,1}, {UC_X86_REG_CS,0xbc,1},
    {UC_X86_REG_EFLAGS,0xc0,1}, {UC_X86_REG_ESP,0xc4,1}, {UC_X86_REG_SS,0xc8,1}
};
struct FP { uint64_t mantissa; uint16_t exponent; };
uint64_t descriptor(uint32_t base, uint32_t limit, uint32_t access, uint32_t flags) {
    return (limit & 0xffffull) | (uint64_t(base & 0xffffff) << 16) | (uint64_t(access) << 40) |
           (uint64_t((limit >> 16) & 15) << 48) | (uint64_t(flags) << 52) | (uint64_t(base >> 24) << 56);
}
uint32_t get(const std::array<uint8_t,contextSize>& b, size_t off) {
    uint32_t v; std::memcpy(&v,b.data()+off,4); return v;
}
void put(std::array<uint8_t,contextSize>& b, size_t off, uint32_t v) { std::memcpy(b.data()+off,&v,4); }
}

struct X86GuestVM::Impl {
    X86GuestVM& owner;
    uc_engine* uc = nullptr;
    uint32_t teb = 0, stackLo = 0, stackHi = 0, handlerStop = 0;
    std::array<uint32_t,6> dr{};
    struct Veh { uint32_t handle, address; };
    std::vector<Veh> veh;
    uint32_t nextVeh = 1;
    ExternalPredicate isExternal;
    ExternalCall external;
    bool running = false, dispatching = false, pending = false, stopped = false;
    // Z-NATIF-15h: no hook runs on every instruction any more (it held the VM at 15-22 M instructions a second).
    // An access that touches an armed data breakpoint cuts its instruction (replay), the write it may already have
    // made is put back (undo) and the instruction runs again, alone, under the trap flag (stepping): the #DB that
    // ends it delivers the breakpoint after retirement, as the processor does.
    bool replay = false, stepping = false, stepped = false;
    struct { uint32_t address = 0, size = 0; std::array<uint8_t,16> bytes; } undo;
    uint32_t externalPc = 0, stop = 0, dataMask = 0;
    BlockPredicate onBlock;
    uint32_t prevEnd = 0, blockStop = 0;
    uint64_t count = 0, exceptions = 0;
    Limits limits;
    std::chrono::steady_clock::time_point deadline;
    std::string error;
    struct Event {
        uint32_t code = 0, flags = 0, address = 0, contextFlags = 0x1003f;
        std::vector<uint32_t> args;
        std::array<uint8_t,contextSize> context{};
    } event;

    // Z-NATIF-15c: the translation cache is kept from one run to the next (flushed before each run, 63 % of the
    // VM's time went to retranslating the same check every server frame). Unicorn keeps the cache true to the
    // writes its own code makes; guest code is also rewritten by code it cannot see (translated functions, between
    // runs or inside an external call). So the pages translations were made from are copied each time the VM hands
    // the thread over and compared when it takes it back: one changed byte flushes everything.
    struct CodePage { std::array<uint8_t,4096> bytes; bool readable; };
    std::unordered_map<uint32_t,CodePage> codePages;        // page number -> content when the VM last stopped
    std::vector<bool> noted=std::vector<bool>(1u<<20);      // pages an instruction has run from since the last flush
    uint32_t lastPage=~0u;
    uint64_t flushes=0, memoryCalls=0, externals=0;
    // BO2_X86_PROF=1 (diagnostic) : un bloc sur 16 compte son adresse ; tous les 2^23 blocs, les 12 premieres.
    std::unordered_map<uint32_t,uint64_t> prof; uint64_t profN=0;   // count est remis a zero a chaque run
    // Data breakpoints: one memory hook per armed breakpoint, over its range only (a hook over everything makes
    // Unicorn resynchronise the CPU on every access: 81 ns against 17, armed or not).
    std::vector<uc_hook> dataHooks;
    std::array<uint32_t,5> dataHooksFor{};                  // Dr0-3 and Dr7 the hooks were placed for

    explicit Impl(X86GuestVM& o) : owner(o) {
        check(uc_open(UC_ARCH_X86,UC_MODE_32,&uc));
        try {
            uc_hook h;
            check(uc_hook_add(uc,&h,UC_HOOK_BLOCK,reinterpret_cast<void*>(blockHook),this,1,0));
            // Matches no 32-bit address and is never removed: blocks translated while a memory hook exists call
            // the hooks on every access, the others need not, and that must not change under a kept cache.
            check(uc_hook_add(uc,&h,UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,reinterpret_cast<void*>(memoryHook),this,~0ull,~0ull));
            check(uc_hook_add(uc,&h,UC_HOOK_INTR,reinterpret_cast<void*>(interruptHook),this,1,0));
        } catch (...) { uc_close(uc); throw; }
    }
    void noteCode(uint32_t page) {      // a translated block starts in the page of its first instruction and
        noted[page]=true;               // ends in the next one at most
        codePages.try_emplace(page);
        if (page<0xfffff) codePages.try_emplace(page+1);
    }
    // P881 : la memoire mappee par pointeur se lit sans Unicorn (uc_mem_read = 100 us par appel externe pour 177
    // pages, 58 en direct) ; enterCode compare sur place, sans copie.
    struct HostRange { uint64_t first, size; uint8_t* host; };
    std::vector<HostRange> hostRanges;
    const uint8_t* hostPage(uint32_t page) const {
        const uint64_t a=uint64_t(page)<<12;
        for (const auto& r:hostRanges) if (a>=r.first && a+4096<=r.first+r.size) return r.host+(a-r.first);
        return nullptr;
    }
    bool readPage(uint32_t page,uint8_t* out) const {
        if (const uint8_t* h=hostPage(page)) { std::memcpy(out,h,4096); return true; }
        return uc_mem_read(uc,uint64_t(page)<<12,out,4096)==UC_ERR_OK;
    }
    void leaveCode() noexcept { for (auto& [page,c]:codePages) c.readable=readPage(page,c.bytes.data()); }
    void enterCode() {
        std::array<uint8_t,4096> now;
        for (auto& [page,c]:codePages) {
            if (const uint8_t* h=hostPage(page)) { if (c.readable && !std::memcmp(h,c.bytes.data(),4096)) continue; }
            else if (readPage(page,now.data())==c.readable && (!c.readable || !std::memcmp(now.data(),c.bytes.data(),4096))) continue;
            check(uc_ctl_flush_tb(uc));
            for (auto& kv:codePages) noted[kv.first]=false;
            codePages.clear(); lastPage=~0u; ++flushes;
            return;
        }
    }
    // Code hooks are narrow (stop addresses, execution breakpoints, external ranges) and never removed: a
    // translation made before its address was hooked does not call them, hence the invalidation; one made since
    // always does, and codeHook reads the state of the moment.
    std::unordered_set<uint64_t> codeHooked;
    // Multi : 171 appels externes par image du serveur x 177 pages copiees puis comparees = 55 ms (run 063434).
    std::unordered_set<uint32_t> codeSafe;
    void hookCode(uint32_t first,uint32_t last) {
        if (!codeHooked.insert(uint64_t(first)<<32|last).second) return;
        uc_hook h;
        check(uc_hook_add(uc,&h,UC_HOOK_CODE,reinterpret_cast<void*>(codeHook),this,first,last));
        check(uc_ctl_remove_cache(uc,uint64_t(first),uint64_t(last)+1));
    }
    void syncDataHooks() {
        const std::array<uint32_t,5> key{dr[0],dr[1],dr[2],dr[3],dr[5]};
        if (key==dataHooksFor) return;
        for (auto h:dataHooks) check(uc_hook_del(uc,h));
        dataHooks.clear(); dataHooksFor=key;
        const uint32_t lengths[]={1,2,8,4};
        for (unsigned i=0;i<4;++i) {
            const unsigned rw=(dr[5]>>(16+4*i))&3;
            if (!((dr[5]>>(2*i))&3) || rw==2) continue;
            if (rw==0) { hookCode(dr[i],dr[i]); continue; }
            // Unicorn bounds a hook by the first byte of the access; match() decides on the overlap.
            const uint64_t first=dr[i]>=0xfff?dr[i]-0xfff:0, last=uint64_t(dr[i])+lengths[(dr[5]>>(18+4*i))&3]-1;
            uc_hook h;
            check(uc_hook_add(uc,&h,UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,reinterpret_cast<void*>(memoryHook),this,first,last));
            dataHooks.push_back(h);
        }
    }
    ~Impl() { uc_close(uc); }
    void fail(const char* why) noexcept { error = why; uc_emu_stop(uc); }
    template<class F> void boundary(F f) noexcept {
        try { f(); } catch (const std::exception& e) { fail(e.what()); }
        catch (...) { fail("unknown host exception"); }
    }
    bool inStack(uint32_t addr, uint32_t size) const {
        return addr >= stackLo && uint64_t(addr)+size <= stackHi;
    }
    void captureFloating(std::array<uint8_t,contextSize>& b) {
        // FLOATING_SAVE_AREA plus FXSAVE-compatible ExtendedRegisters.
        const uint32_t cw=owner.reg(UC_X86_REG_FPCW), sw=owner.reg(UC_X86_REG_FPSW);
        const uint32_t tag=owner.reg(UC_X86_REG_FPTAG);
        put(b,0x1c,cw); put(b,0x20,sw); put(b,0x24,tag);
        put(b,0x28,owner.reg(UC_X86_REG_FIP)); put(b,0x2c,owner.reg(UC_X86_REG_FCS));
        put(b,0x30,owner.reg(UC_X86_REG_FDP)); put(b,0x34,owner.reg(UC_X86_REG_FDS));
        uint16_t cw16=cw, sw16=sw;
        std::memcpy(b.data()+0xcc,&cw16,2); std::memcpy(b.data()+0xce,&sw16,2);
        uint8_t abridged=0;
        for (unsigned i=0;i<8;++i) if (((tag>>(2*i))&3)!=3) abridged |= 1u<<i;
        b[0xd0]=abridged;
        put(b,0xd4,owner.reg(UC_X86_REG_FIP)); put(b,0xd8,owner.reg(UC_X86_REG_FCS));
        put(b,0xdc,owner.reg(UC_X86_REG_FDP)); put(b,0xe0,owner.reg(UC_X86_REG_FDS));
        put(b,0xe4,owner.reg(UC_X86_REG_MXCSR)); put(b,0xe8,0xffff);
        const unsigned top=(sw>>11)&7;
        for (unsigned i=0;i<8;++i) {
            FP fp{}; owner.readReg(UC_X86_REG_FP0+((top+i)&7),&fp);
            std::memcpy(b.data()+0x38+10*i,&fp.mantissa,8);
            std::memcpy(b.data()+0x40+10*i,&fp.exponent,2);
            std::memcpy(b.data()+0xec+16*i,&fp.mantissa,8);
            std::memcpy(b.data()+0xf4+16*i,&fp.exponent,2);
            owner.readReg(UC_X86_REG_XMM0+i,b.data()+0x16c+16*i);
        }
    }
    void capture(uint32_t code, uint32_t flags, uint32_t addr, uint32_t cf=0x1003f) {
        if (dispatching || pending) throw std::runtime_error("nested exception is not supported");
        event = Event{}; event.code=code; event.flags=flags; event.address=addr; event.contextFlags=cf;
        put(event.context,0,cf);
        for (auto f:fields) put(event.context,f.offset,owner.reg(f.id));
        for (unsigned i=0;i<6;++i) put(event.context,4+4*i,dr[i]);
        captureFloating(event.context);
        pending=true;
    }
    uint32_t match(uint32_t address, unsigned size, bool write) const {
        uint32_t mask=0;
        const uint32_t lengths[]={1,2,8,4};
        for (unsigned i=0;i<4;++i) {
            if (!((dr[5]>>(2*i))&3)) continue;
            unsigned rw=(dr[5]>>(16+4*i))&3;
            if (rw==0 || rw==2 || (rw==1 && !write)) continue;
            unsigned n=lengths[(dr[5]>>(18+4*i))&3];
            if (uint64_t(address)<uint64_t(dr[i])+n && uint64_t(dr[i])<uint64_t(address)+size) mask|=1u<<i;
        }
        return mask;
    }
    static void memoryHook(uc_engine*, uc_mem_type type, uint64_t address, int size, int64_t, void* opaque) noexcept {
        auto& p=*static_cast<Impl*>(opaque);
        ++p.memoryCalls;
        p.boundary([&] {
            uint32_t mask=p.match(uint32_t(address),unsigned(size),type==UC_MEM_WRITE);
            if (!mask) return;
            if (p.dispatching) throw std::runtime_error("nested data breakpoint is not supported");
            if (p.stepping) { p.dataMask |= mask; return; } // the instruction retires; delivered at the #DB of its step
            // Unicorn stops at once: a read has changed nothing; a write is already in memory, the rest of the
            // instruction (flags, registers, EIP) is not. Keep the bytes it replaces (the hook runs before it).
            if (type==UC_MEM_WRITE) {
                if (size_t(size)>p.undo.bytes.size()) throw std::runtime_error("watched write wider than 16 bytes");
                p.undo.address=uint32_t(address); p.undo.size=uint32_t(size);
                p.owner.read(p.undo.address,p.undo.bytes.data(),p.undo.size);
            }
            p.replay=true; uc_emu_stop(p.uc);
        });
    }
    // Runs at the start of every translated block: code pages, budget, deadline.
    static void blockHook(uc_engine*, uint64_t address, uint32_t size, void* opaque) noexcept {
        auto& p=*static_cast<Impl*>(opaque);
        p.boundary([&] {
            const uint32_t page=uint32_t(address)>>12;
            if (page!=p.lastPage) { p.lastPage=page; if (!p.noted[page]) p.noteCode(page); }
            if (++p.count>p.limits.blocks) throw std::runtime_error("block budget exceeded");
            if (p.onBlock) {
                const uint32_t pc=uint32_t(address), end=p.prevEnd; p.prevEnd=pc+size;
                // Unicorn 2.1.4 emits this hook before the exit check of the block (translator.c): the stop lands
                // before its first instruction. execute() checks it.
                switch (p.onBlock(pc,end)) {
                case BlockAction::Continue: break;
                case BlockAction::External: p.externalPc=pc; p.blockStop=pc; p.prevEnd=0; uc_emu_stop(p.uc); return;   // ce bloc ne tourne pas
                case BlockAction::Stop: p.stopped=true; p.blockStop=pc; uc_emu_stop(p.uc); return;
                }
            }
            static const bool profOn=std::getenv("BO2_X86_PROF")!=nullptr;
            if (profOn && (p.count&15)==0) {
                ++p.prof[uint32_t(address)];
                if ((++p.profN&((1u<<19)-1))==0) {
                    std::vector<std::pair<uint64_t,uint32_t>> top;
                    for (auto& [a,n]:p.prof) top.push_back({n,a});
                    std::sort(top.rbegin(),top.rend());
                    std::fprintf(stderr,"[X86-PROF] %zu adresses, tete :",top.size());
                    for (size_t i=0;i<top.size() && i<12;++i) std::fprintf(stderr," %08x=%llu",top[i].second,(unsigned long long)top[i].first);
                    std::fprintf(stderr,"\n");
                }
            }
            if ((p.count&4095)==0 && std::chrono::steady_clock::now()>=p.deadline)
                throw std::runtime_error("wall-time budget exceeded");
        });
    }
    // Runs before the instruction, at hooked addresses only (hookCode).
    static void codeHook(uc_engine*, uint64_t address, uint32_t, void* opaque) noexcept {
        auto& p=*static_cast<Impl*>(opaque);
        p.boundary([&] {
            uint32_t pc=uint32_t(address);
            if (pc==p.stop) { p.stopped=true; uc_emu_stop(p.uc); return; }
            for (unsigned i=0;i<4;++i) {
                if (!((p.dr[5]>>(2*i))&3) || ((p.dr[5]>>(16+4*i))&3) || p.dr[i]!=pc) continue;
                if (p.dispatching) throw std::runtime_error("nested execution breakpoint is not supported");
                if (p.owner.reg(UC_X86_REG_EFLAGS)&0x10000) throw std::runtime_error("RF suppression is not supported");
                p.dr[4]=(p.dr[4]&~0xfu)|(1u<<i); p.capture(singleStep,0,pc); uc_emu_stop(p.uc); return;
            }
            if (p.isExternal && p.isExternal(pc)) { p.externalPc=pc; uc_emu_stop(p.uc); }
        });
    }
    static void interruptHook(uc_engine*, uint32_t no, void* opaque) noexcept {
        auto& p=*static_cast<Impl*>(opaque);
        p.boundary([&] {
            if (no==1 && p.stepping) { p.stepping=false; p.stepped=true; uc_emu_stop(p.uc); return; }
            if (no!=3) throw std::runtime_error("unsupported x86 interrupt");
            uint32_t addr=p.owner.reg(UC_X86_REG_EIP)-1; uint8_t op;
            p.owner.read(addr,&op,1);
            if (op!=0xcc) throw std::runtime_error("unsupported breakpoint encoding");
            p.capture(breakpoint,0,addr);
            put(p.event.context,0xb8,addr); // dispatcher exposes the INT3 itself
            uc_emu_stop(p.uc);
        });
    }
    void beginStep() {
        replay=false;
        if (undo.size) { owner.write(undo.address,undo.bytes.data(),undo.size); undo.size=0; }
        // The trap flag must be neither read nor replaced by the instruction it steps.
        uint8_t code[4]{}; owner.read(owner.reg(UC_X86_REG_EIP),code,sizeof code);
        const auto prefix=[](uint8_t b) { return b==0x66||b==0x67||b==0xf0||b==0xf2||b==0xf3||b==0x26||b==0x2e||b==0x36||b==0x3e||b==0x64||b==0x65; };
        unsigned i=0; while (i<3 && prefix(code[i])) ++i;
        if (code[i]==0x9c || code[i]==0x9d || code[i]==0xcf)
            throw std::runtime_error("data breakpoint on pushf/popf/iret is not supported");
        stepping=true; owner.reg(UC_X86_REG_EFLAGS,owner.reg(UC_X86_REG_EFLAGS)|0x100);
    }
    void endStep() {
        stepped=false; owner.reg(UC_X86_REG_EFLAGS,owner.reg(UC_X86_REG_EFLAGS)&~0x100u);
        if (!dataMask) return;
        // B0-B3 name the breakpoints of THIS trap: the CPU clears them on a breakpoint #DB and 64-bit
        // Windows zeroes DR6 on return to user mode. 0x9ad430 tests bit 0 first, so a kept bit misroutes it.
        dr[4]=(dr[4]&~0xfu)|dataMask; dataMask=0;
        capture(singleStep,0,owner.reg(UC_X86_REG_EIP));
    }
    void execute(uint32_t target) {
        stop=target; stopped=false;
        for (;;) {
            if (!error.empty()) throw std::runtime_error(error);
            if (pending) { dispatch(); continue; }
            if (stopped) return;
            if (std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("wall-time budget exceeded");
            externalPc=0;
            if (replay) beginStep();
            // No timeout argument: Unicorn starts a timer thread at every call; blockHook watches the deadline.
            uc_err e=uc_emu_start(uc,owner.reg(UC_X86_REG_EIP),0,0,0);
            if (!error.empty()) throw std::runtime_error(error);
            check(e);
            if (blockStop) {
                if (owner.reg(UC_X86_REG_EIP)!=blockStop) throw std::runtime_error("block stop ran the block");
                blockStop=0;
            }
            if (stepped) { endStep(); continue; }
            if (stepping) throw std::runtime_error("single step did not complete");
            if (pending || stopped || replay) continue;
            if (externalPc) {
                if (!external) throw std::runtime_error("external target has no adapter");
                const uint32_t before=externalPc;
                const bool safe=codeSafe.count(before)!=0;
                ++externals; if (!safe) leaveCode();
                external(owner,before); // outside emu_start, scheduler may suspend here
                if (!safe) enterCode();
                if (!pending && owner.reg(UC_X86_REG_EIP)==before)
                    throw std::runtime_error("external adapter did not advance EIP");
                continue;
            }
            throw std::runtime_error("unexpected stop");
        }
    }
    uint32_t invoke(uint32_t address, const std::vector<uint32_t>& args, uint32_t sp) {
        uc_context* saved=nullptr; check(uc_context_alloc(uc,&saved));
        std::unique_ptr<uc_context,decltype(&uc_context_free)> ctx(saved,uc_context_free);
        check(uc_context_save(uc,saved));
        uint32_t outerStop=stop, chain=owner.read32(teb); bool outerStopped=stopped;
        try {
            owner.write32(sp,handlerStop);
            for (size_t i=0;i<args.size();++i) owner.write32(sp+4+uint32_t(i)*4,args[i]);
            owner.reg(UC_X86_REG_ESP,sp); owner.reg(UC_X86_REG_EIP,address);
            execute(handlerStop);
            uint32_t result=owner.reg(UC_X86_REG_EAX);
            if (owner.read32(teb)!=chain) throw std::runtime_error("handler changed SEH chain; unwind unsupported");
            check(uc_context_restore(uc,saved)); stop=outerStop; stopped=outerStopped;
            return result;
        } catch (...) {
            uc_context_restore(uc,saved); stop=outerStop; stopped=outerStopped; throw;
        }
    }
    void apply(const std::array<uint8_t,contextSize>& original, uint32_t ptr) {
        std::array<uint8_t,contextSize> b; owner.read(ptr,b.data(),b.size());
        const uint32_t flags=get(b,0);
        // Keep the complete saved CPU state. Handler modifications to x87/SSE
        // CONTEXT need a separate restore implementation; never silently drop them.
        if ((flags&8) && std::memcmp(b.data()+0x1c,original.data()+0x1c,0x70))
            throw std::runtime_error("modified floating-point CONTEXT is not supported");
        if ((flags&0x20) && std::memcmp(b.data()+0xcc,original.data()+0xcc,512))
            throw std::runtime_error("modified extended CONTEXT is not supported");
        for (auto f:fields) if (flags&f.group) owner.reg(f.id,get(b,f.offset));
        if (flags&0x10) { for (unsigned i=0;i<6;++i) dr[i]=get(b,4+4*i); syncDataHooks(); }
    }
    void dispatch() {
        if (dispatching) throw std::runtime_error("nested exception is not supported");
        pending=false; ++exceptions;
        Event ev=event;
        // Z-NATIF-08s (diagnostic): the game's handlers hash Dr0-3/Dr7 and relay only on a match (0x542ff0).
        static const bool trace=std::getenv("BO2_UNWIND_LOG")!=nullptr;
        const auto before=dr;
        uint32_t suspended=get(ev.context,0xc4);
        if (suspended<0x1000 || !inStack(suspended-0x1000,0x1000))
            throw std::runtime_error("insufficient guest exception stack");
        const uint32_t sp=(suspended-0x800)&~15u, context=sp+0x200, record=sp+0x500;
        const uint32_t pointers=sp+0x580, dispatcher=sp+0x590;
        uint8_t zeros[0x100]{}; owner.write(record,zeros,sizeof zeros);
        owner.write32(record,ev.code); owner.write32(record+4,ev.flags); owner.write32(record+12,ev.address);
        owner.write32(record+16,uint32_t(ev.args.size()));
        for (size_t i=0;i<ev.args.size();++i) owner.write32(record+20+uint32_t(i)*4,ev.args[i]);
        owner.write(context,ev.context.data(),ev.context.size());
        owner.write32(pointers,record); owner.write32(pointers+4,context);
        dispatching=true;
        bool handled=false;
        try {
            // Snapshot: registration/removal during invocation must not invalidate iteration.
            const auto handlers=veh;
            for (auto h:handlers) {
                uint32_t r=invoke(h.address,{pointers},sp);
                if (r==0xffffffff) { handled=true; break; }
                if (r!=0) throw std::runtime_error("unsupported VEH disposition");
            }
            uint32_t frame=owner.read32(teb);
            std::vector<uint32_t> seen;
            while (!handled && frame!=0xffffffff) {
                if (seen.size()>=32 || std::find(seen.begin(),seen.end(),frame)!=seen.end())
                    throw std::runtime_error("SEH chain cycle or limit");
                if (!inStack(frame,8)) throw std::runtime_error("SEH frame outside guest stack");
                seen.push_back(frame);
                uint32_t next=owner.read32(frame), handler=owner.read32(frame+4);
                uint32_t r=invoke(handler,{record,frame,context,dispatcher},sp);
                if (r==0) handled=true;
                else if (r!=1) throw std::runtime_error("unsupported SEH disposition or unwind");
                frame=next;
            }
            if (!handled) throw std::runtime_error("unhandled guest exception");
            if ((ev.flags&1) && (owner.read32(record+4)&1))
                throw std::runtime_error("noncontinuable guest exception");
            apply(ev.context,context);
            dispatching=false;
            if (trace) std::fprintf(stderr,"[X86-EXC] code=%08x addr=%08x eip=%08x->%08x dr=%08x,%08x,%08x,%08x dr6=%08x dr7=%08x"
                " -> dr=%08x,%08x,%08x,%08x dr6=%08x dr7=%08x\n",ev.code,ev.address,get(ev.context,0xb8),owner.reg(UC_X86_REG_EIP),
                before[0],before[1],before[2],before[3],before[4],before[5],dr[0],dr[1],dr[2],dr[3],dr[4],dr[5]);
        } catch (...) { dispatching=false; throw; }
    }
};

X86GuestVM::X86GuestVM() : p(new Impl(*this)) {}
X86GuestVM::~X86GuestVM() = default;
void X86GuestVM::map(uint32_t a,uint64_t n,void* b) {
    check(b?uc_mem_map_ptr(p->uc,a,n,UC_PROT_ALL,b):uc_mem_map(p->uc,a,n,UC_PROT_ALL));
    if (b) p->hostRanges.push_back({a,n,static_cast<uint8_t*>(b)});
}
void X86GuestVM::read(uint32_t a,void* b,size_t n) const { check(uc_mem_read(p->uc,a,b,n)); }
void X86GuestVM::write(uint32_t a,const void* b,size_t n) { check(uc_mem_write(p->uc,a,b,n)); }
uint32_t X86GuestVM::read32(uint32_t a) const { uint32_t v; read(a,&v,4); return v; }
void X86GuestVM::write32(uint32_t a,uint32_t v) { write(a,&v,4); }
uint32_t X86GuestVM::reg(int id) const { uint64_t v=0; readReg(id,&v); return uint32_t(v); }
void X86GuestVM::reg(int id,uint32_t v) { uint64_t wide=v; writeReg(id,&wide); }
void X86GuestVM::readReg(int id,void* out) const { check(uc_reg_read(p->uc,id,out)); }
void X86GuestVM::writeReg(int id,const void* in) { check(uc_reg_write(p->uc,id,in)); }
void X86GuestVM::configureThread(uint32_t gdt,uint32_t teb,uint32_t lo,uint32_t hi) {
    if (p->running || lo>=hi) throw std::runtime_error("invalid thread configuration");
    uint64_t flat=descriptor(0,0xfffff,0x93,0xc), fs=descriptor(teb,0xfff,0x93,4);
    write(gdt+16,&flat,8); write(gdt+24,&fs,8);
    uc_x86_mmr gdtr{0,gdt,0x100,0}; writeReg(UC_X86_REG_GDTR,&gdtr);
    for (int id:{UC_X86_REG_CS,UC_X86_REG_SS,UC_X86_REG_DS,UC_X86_REG_ES}) reg(id,16);
    reg(UC_X86_REG_FS,24); reg(UC_X86_REG_EFLAGS,0x202);
    p->teb=teb; p->stackLo=lo; p->stackHi=hi; p->handlerStop=gdt+0xff0;
    const uint8_t ret=0xc3; write(p->handlerStop,&ret,1);
    p->hookCode(p->handlerStop,p->handlerStop);
}
void X86GuestVM::externalCalls(ExternalPredicate pred,ExternalCall call,const std::vector<std::pair<uint32_t,uint32_t>>& ranges) {
    p->isExternal=std::move(pred); p->external=std::move(call);
    for (auto [first,last]:ranges) p->hookCode(first,last);
}
void X86GuestVM::blockCalls(BlockPredicate pred) { p->onBlock=std::move(pred); }
void X86GuestVM::codeSafeExternal(uint32_t a) { p->codeSafe.insert(a); }
uint32_t X86GuestVM::addVeh(bool first,uint32_t a) {
    uint32_t id=p->nextVeh++;
    if (!id || !a) throw std::runtime_error("invalid VEH handle or address");
    if (first) p->veh.insert(p->veh.begin(),{id,a}); else p->veh.push_back({id,a});
    return id;
}
bool X86GuestVM::removeVeh(uint32_t h) {
    auto it=std::find_if(p->veh.begin(),p->veh.end(),[&](auto v){return v.handle==h;});
    if (it==p->veh.end()) return false; p->veh.erase(it); return true;
}
void X86GuestVM::debugRegisters(const std::array<uint32_t,6>& dr) { p->dr=dr; }
const std::array<uint32_t,6>& X86GuestVM::debugRegisters() const { return p->dr; }
void X86GuestVM::raiseException(uint32_t code,uint32_t flags,uint32_t n,uint32_t args) {
    if (!args) n=0; // Win32 ignores NumberOfArguments when lpArguments is NULL.
    if (!p->running || n>15) throw std::runtime_error("invalid RaiseException invocation");
    uint32_t esp=reg(UC_X86_REG_ESP), ret=read32(esp);
    p->capture(code,flags,ret,0x10007);
    for (uint32_t i=0;i<n;++i) p->event.args.push_back(read32(args+4*i));
    put(p->event.context,0xb8,ret); put(p->event.context,0xc4,esp+20);
}
X86GuestVM::Result X86GuestVM::run(uint32_t entry,uint32_t stop,Limits limits) {
    Result r;
    if (p->running) { r.error="reentrant VM run"; return r; }
    p->running=true; p->error.clear(); p->count=0; p->exceptions=0; p->pending=false; p->dataMask=0;
    p->replay=p->stepping=p->stepped=false; p->undo.size=0; p->prevEnd=p->blockStop=0;
    p->limits=limits; p->deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(limits.milliseconds);
    const uint64_t flushes=p->flushes, memoryCalls=p->memoryCalls, externals=p->externals;
    try {
        if (!p->teb || !limits.blocks || !limits.milliseconds) throw std::runtime_error("unconfigured VM or empty budget");
        reg(UC_X86_REG_EIP,entry);
        p->enterCode(); p->syncDataHooks(); p->hookCode(stop,stop);
        p->execute(stop); r.returned=true;
    } catch (const std::exception& e) { r.error=e.what(); }
    p->leaveCode();
    r.pc=reg(UC_X86_REG_EIP); r.blocks=p->count; r.exceptions=p->exceptions;
    r.flushes=p->flushes-flushes; r.memoryCalls=p->memoryCalls-memoryCalls; r.externals=p->externals-externals;
    r.codePages=p->codePages.size();
    p->running=false; return r;
}
} // namespace bo2
