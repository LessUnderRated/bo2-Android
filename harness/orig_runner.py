"""
OrigRunner — the "original binary" side of the differential harness.

Executes x86-32 machine code under Unicorn Engine with full architectural
state access, and snapshots GPRs + flags + defined memory regions at
configured boundaries.

This is the foundation for the "original" side described in docs/HARNESS.md.
For the toy proof-of-life it runs a single synthetic function; the real
harness will drive this with replay corpora and snapshot at every external
call / function boundary.
"""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
    UC_X86_REG_EIP, UC_X86_REG_EFLAGS,
)

# x86-32 GPR snapshot order
_GPRS = {
    "eax": UC_X86_REG_EAX, "ebx": UC_X86_REG_EBX, "ecx": UC_X86_REG_ECX,
    "edx": UC_X86_REG_EDX, "esi": UC_X86_REG_ESI, "edi": UC_X86_REG_EDI,
    "ebp": UC_X86_REG_EBP, "esp": UC_X86_REG_ESP, "eip": UC_X86_REG_EIP,
    "eflags": UC_X86_REG_EFLAGS,
}


@dataclass
class State:
    """Architectural state snapshot of the x86-32 side."""
    regs: Dict[str, int] = field(default_factory=dict)
    mem: Dict[int, bytes] = field(default_factory=dict)  # addr -> bytes

    def diff(self, other: "State") -> Dict[str, object]:
        """Return a diff dict: only fields that differ."""
        out: Dict[str, object] = {}
        for k, v in self.regs.items():
            ov = other.regs.get(k)
            if v != ov:
                out[k] = {"orig": v, "recomp": ov}
        for addr, b in self.mem.items():
            ob = other.mem.get(addr)
            if b != ob:
                out[f"mem@{addr:#x}"] = {"orig": b, "recomp": ob}
        return out


class OrigRunner:
    """Unicorn-based x86-32 executor with state snapshotting."""

    def __init__(self, code_addr: int = 0x1000, code_size: int = 0x1000,
                 stack_addr: int = 0x3000, stack_size: int = 0x1000,
                 ret_addr: int = 0x2000):
        self.code_addr = code_addr
        self.code_size = code_size
        self.stack_addr = stack_addr
        self.stack_size = stack_size
        self.ret_addr = ret_addr  # sentinel EIP that stops execution
        self.uc: Optional[Uc] = None
        self._stop = False

    def _init_uc(self) -> Uc:
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(self.code_addr, self.code_size)
        uc.mem_map(self.ret_addr, 0x1000)        # landing pad for ret
        uc.mem_map(self.stack_addr, self.stack_size)
        return uc

    def _hook_code(self, uc, addr, size, _ud):
        if addr == self.ret_addr:
            self._stop = True
            uc.emu_stop()

    def _hook_mem_invalid(self, uc, access, addr, size, value, _ud):
        raise RuntimeError(f"unmapped memory access at {addr:#x} (access={access})")

    def load_code(self, code: bytes) -> None:
        self.uc = self._init_uc()
        self.uc.mem_write(self.code_addr, code)
        self.uc.hook_add(UC_HOOK_CODE, self._hook_code)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self._hook_mem_invalid)

    def set_arg_regs(self, regs: Dict[str, int]) -> None:
        assert self.uc is not None, "call load_code first"
        for name, val in regs.items():
            self.uc.reg_write(_GPRS[name], val & 0xFFFFFFFF)

    def setup_ret(self) -> None:
        """Push the sentinel return address onto the stack and set ESP.

        push semantics: ESP -= 4, then [ESP] = value. `ret` then reads [ESP],
        adds 4 to ESP, and jumps to the popped address.
        """
        assert self.uc is not None
        sp = self.stack_addr + self.stack_size - 0x10
        sp -= 4                                       # allocate the return slot
        self.uc.mem_write(sp, self.ret_addr.to_bytes(4, "little"))
        self.uc.reg_write(UC_X86_REG_ESP, sp)

    def run(self, until: Optional[int] = None, count: int = 10_000_000) -> None:
        assert self.uc is not None
        self._stop = False
        start = self.uc.reg_read(UC_X86_REG_EIP)
        try:
            self.uc.emu_start(start, until if until is not None else self.ret_addr,
                              count=count)
        except Exception as e:
            raise RuntimeError(f"Unicorn execution failed: {e}") from e

    def snapshot(self, mem_regions: Optional[List[tuple]] = None) -> State:
        assert self.uc is not None
        regs = {name: self.uc.reg_read(const) for name, const in _GPRS.items()}
        mem: Dict[int, bytes] = {}
        if mem_regions:
            for addr, size in mem_regions:
                mem[addr] = self.uc.mem_read(addr, size)
        return State(regs=regs, mem=mem)

    @staticmethod
    def regs_to_signed(state: State, names=("eax", "ebx", "ecx", "edx")) -> Dict[str, int]:
        out = {}
        for n in names:
            v = state.regs.get(n, 0)
            out[n] = v - 0x100000000 if v & 0x80000000 else v
        return out
