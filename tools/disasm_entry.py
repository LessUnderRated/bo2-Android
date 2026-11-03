"""
disasm_entry — disassemble the t6sp.exe entry function and identify external
calls within it. This defines Tranche 0: entry -> first external call.

Reads the real binary, maps the entry point's bytes, walks instructions until
it hits either:
  - a direct call to an imported function (matched against the PE import table)
  - an indirect call through an IAT slot
  - a ret / unconditional jump that ends the function
  - an instruction cap (safety)

Reports: the instruction trace, the first external call (if any), and the
byte range to lift.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_CALL, CS_GRP_JUMP, CS_OP_IMM, CS_OP_MEM
from pe_parser import PEFile

BIN = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"
MAX_INSNS = 400
MAX_BYTES = 0x800


def build_iat_map(pe):
    """VA -> (dll, func_name) for every IAT slot."""
    m = {}
    for imp in pe.imports:
        for t in imp.thunks:
            va = pe.image_base + t.iat_rva
            m[va] = (imp.dll, t.name or f"#{t.ordinal}")
    return m


def main():
    pe = PEFile(BIN).parse()
    iat = build_iat_map(pe)
    entry_va = pe.image_base + pe.entry_point_rva
    entry_off = pe.rva_to_offset(pe.entry_point_rva)

    print(f"binary: {BIN}")
    print(f"image_base: {pe.image_base:#x}")
    print(f"entry VA: {entry_va:#x}  (file offset {entry_off:#x})")
    print(f"IAT slots: {len(iat)}")
    print()

    raw = pe._raw
    # disassemble
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    code = raw[entry_off:entry_off + MAX_BYTES]
    base_va = entry_va

    print(f"--- first {MAX_INSNS} instructions from entry ---")
    trace = []
    first_ext_call = None
    n = 0
    for ins in md.disasm(code, base_va):
        n += 1
        line = f"{ins.address:#010x}: {ins.bytes.hex():<20s} {ins.mnemonic:<8s} {ins.op_str}"
        trace.append((ins.address, ins.bytes, ins.mnemonic, ins.op_str))

        # identify external call: direct call to IAT slot, or indirect call via IAT
        is_external = False
        ext_target = None
        if ins.mnemonic == "call":
            # direct call: imm operand
            for op in ins.operands:
                if op.type == CS_OP_IMM:
                    tgt = op.imm
                    if tgt in iat:
                        is_external = True
                        ext_target = iat[tgt]
                elif op.type == CS_OP_MEM:
                    # indirect via [disp] -- likely IAT
                    disp = op.mem.disp
                    # absolute address from disp (x86-32 with no base/index in RIP-less mode)
                    if disp in iat:
                        is_external = True
                        ext_target = iat[disp]
        if is_external and first_ext_call is None:
            first_ext_call = (ins.address, ext_target, ins.bytes)
            print(f"{line}   <-- EXTERNAL CALL: {ext_target[0]}!{ext_target[1]}")
        else:
            print(line)

        if n >= MAX_INSNS:
            print(f"...stopped at {MAX_INSNS} instructions")
            break
        # stop at ret (function end) if we've passed the first external call
        if ins.mnemonic == "ret" and first_ext_call is not None:
            print(f"...ret reached, function boundary")
            break

    print()
    if first_ext_call:
        addr, (dll, fn), _ = first_ext_call
        nbytes = (addr + 5) - entry_va  # approx: up to and including the call
        end_addr = addr + 5
        print(f"=== Tranche 0 target ===")
        print(f"entry VA:        {entry_va:#x}")
        print(f"first ext call:  {addr:#x}  -> {dll}!{fn}")
        print(f"byte range:      {entry_va:#x} .. {end_addr:#x}  ({nbytes} bytes)")
        print()
        print(f"To lift: {nbytes} bytes from file offset {entry_off:#x}")
    else:
        print("No external call found in first {} instructions.".format(MAX_INSNS))
        print("Entry may be a short stub that jumps elsewhere; need to follow jumps.")


if __name__ == "__main__":
    main()
