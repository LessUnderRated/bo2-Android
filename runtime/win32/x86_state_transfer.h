#pragma once
// Remill State <-> X86GuestVM, both ways: GPRs, EFLAGS, x87 (ST0-7, control/status/tag words), MXCSR, XMM0-7.
// Shared by the exception adapter (exception_adapter.cpp) and the original-byte interpreter (interp.cpp, every target).
#include "win32_shim.h"
#include "x86_guest_vm.h"
#include <unicorn/x86.h>
#include <cstring>

namespace bo2 {
inline const int gprIds[]={UC_X86_REG_EAX,UC_X86_REG_EBX,UC_X86_REG_ECX,UC_X86_REG_EDX,
                           UC_X86_REG_ESI,UC_X86_REG_EDI,UC_X86_REG_EBP,UC_X86_REG_ESP};
inline Reg* gpr(State& s,unsigned i) {
    Reg* r[]={&s.gpr.rax,&s.gpr.rbx,&s.gpr.rcx,&s.gpr.rdx,&s.gpr.rsi,&s.gpr.rdi,&s.gpr.rbp,&s.gpr.rsp};
    return r[i];
}
struct FP { uint64_t mantissa; uint16_t exponent; };
// Remill keeps logical ST0..7; Unicorn's FP0..7 are physical x87 registers.
inline void toVm(X86GuestVM& vm,State& s,uint32_t pc) {
    for (unsigned i=0;i<8;++i) vm.reg(gprIds[i],gpr(s,i)->dword);
    uint32_t flags=uint32_t(s.rflag.flat)|0x202;
    constexpr uint32_t arithmetic=0x1|0x4|0x10|0x40|0x80|0x400|0x800;
    flags=(flags&~arithmetic)|(s.aflag.cf?1:0)|(s.aflag.pf?4:0)|(s.aflag.af?0x10:0)|
        (s.aflag.zf?0x40:0)|(s.aflag.sf?0x80:0)|(s.aflag.df?0x400:0)|(s.aflag.of?0x800:0);
    vm.reg(UC_X86_REG_EFLAGS,flags);
    auto status=s.x87.fxsave.swd;
    status.c0=s.sw.c0; status.c1=s.sw.c1; status.c2=s.sw.c2; status.c3=s.sw.c3;
    status.ie=s.sw.ie; status.de=s.sw.de; status.ze=s.sw.ze; status.oe=s.sw.oe;
    status.ue=s.sw.ue; status.pe=s.sw.pe; status.sf=s.sw.sf;
    vm.reg(UC_X86_REG_FPCW,s.x87.fxsave.cwd.flat);
    vm.reg(UC_X86_REG_FPSW,status.flat);
    vm.reg(UC_X86_REG_MXCSR,s.x87.fxsave.mxcsr.flat);
    uint32_t tag=0;
    for (unsigned i=0;i<8;++i) {
        vm.writeReg(UC_X86_REG_XMM0+i,&s.vec[i].xmm);
        FP fp{}; static_assert(sizeof(s.st.elems[i].val)==10,"x87 storage");
        std::memcpy(&fp,&s.st.elems[i].val,10);
        vm.writeReg(UC_X86_REG_FP0+((status.top+i)&7),&fp);
        // Empty/full is all the Remill FXSAVE representation retains.
        if (!(s.x87.fxsave.ftw.flat&(1u<<i))) tag|=3u<<(2*i);
    }
    vm.reg(UC_X86_REG_FPTAG,tag);
    vm.reg(UC_X86_REG_EIP,pc);
}
inline void fromVm(X86GuestVM& vm,State& s) {
    for (unsigned i=0;i<8;++i) gpr(s,i)->dword=vm.reg(gprIds[i]);
    uint32_t f=vm.reg(UC_X86_REG_EFLAGS); s.rflag.flat=f;
    s.aflag.cf=f&1; s.aflag.pf=(f>>2)&1; s.aflag.af=(f>>4)&1; s.aflag.zf=(f>>6)&1;
    s.aflag.sf=(f>>7)&1; s.aflag.df=(f>>10)&1; s.aflag.of=(f>>11)&1;
    s.x87.fxsave.cwd.flat=vm.reg(UC_X86_REG_FPCW);
    s.x87.fxsave.swd.flat=vm.reg(UC_X86_REG_FPSW);
    s.x87.fxsave.mxcsr.flat=vm.reg(UC_X86_REG_MXCSR);
    const auto status=s.x87.fxsave.swd;
    s.sw.c0=status.c0; s.sw.c1=status.c1; s.sw.c2=status.c2; s.sw.c3=status.c3;
    s.sw.ie=status.ie; s.sw.de=status.de; s.sw.ze=status.ze; s.sw.oe=status.oe;
    s.sw.ue=status.ue; s.sw.pe=status.pe; s.sw.sf=status.sf;
    uint32_t tag=vm.reg(UC_X86_REG_FPTAG); s.x87.fxsave.ftw.flat=0;
    for (unsigned i=0;i<8;++i) {
        vm.readReg(UC_X86_REG_XMM0+i,&s.vec[i].xmm);
        FP fp{}; vm.readReg(UC_X86_REG_FP0+((status.top+i)&7),&fp);
        std::memcpy(&s.st.elems[i].val,&fp,10);
        FP physical{}; vm.readReg(UC_X86_REG_FP0+i,&physical);
        std::memcpy(&s.mmx.elems[i].val,&physical.mantissa,8);
        if (((tag>>(2*i))&3)!=3) s.x87.fxsave.ftw.flat|=1u<<i;
    }
    s.gpr.rip.dword=vm.reg(UC_X86_REG_EIP); s._0=s.gpr.rip.dword;
}
} // namespace bo2
