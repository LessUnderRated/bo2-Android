// FindCallers.java — find functions that call a given target function.
// Returns caller candidates with size and call structure, so we can pick
// a Tranche 1 target = a small function calling our Tranche 0 target.
//@category BO2

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

public class FindCallers extends GhidraScript {

    private Set<Address> findCallees(Function func, ReferenceManager rm, FunctionManager fm) {
        Set<Address> callees = new LinkedHashSet<>();
        AddressIterator it = func.getBody().getAddresses(true);
        while (it.hasNext()) {
            Address a = it.next();
            for (Reference cref : rm.getReferencesFrom(a)) {
                if (cref.getReferenceType().isCall()) {
                    Address to = cref.getToAddress();
                    if (fm.getFunctionAt(to) != null) {
                        callees.add(to);
                    }
                }
            }
        }
        return callees;
    }

    private String dumpBytes(Memory mem, Address entry, long size) throws Exception {
        StringBuilder hex = new StringBuilder();
        for (long i = 0; i < size; i++) {
            byte b = mem.getByte(entry.add(i));
            hex.append(String.format("%02x", b & 0xff));
        }
        return hex.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = args.length > 0 ? args[0] : "/tmp/ghidra-out";
        String targetHex = args.length > 1 ? args[1] : "0x9ab420";
        long targetOffset = Long.decode(targetHex);
        new java.io.File(outDir).mkdirs();

        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        Memory mem = currentProgram.getMemory();
        AddressFactory af = currentProgram.getAddressFactory();

        Address targetAddr = af.getDefaultAddressSpace().getAddress(targetOffset);
        Function targetFunc = fm.getFunctionAt(targetAddr);

        PrintWriter pw = new PrintWriter(new FileWriter(outDir + "/callers.txt"));
        pw.println("=== Callers of " + targetHex + " ===");
        if (targetFunc != null) {
            pw.println("target: " + targetFunc.getName() + " size=" +
                    targetFunc.getBody().getNumAddresses());
        }

        List<Reference> callRefs = new ArrayList<>();
        for (Reference r : rm.getReferencesTo(targetAddr)) {
            if (r.getReferenceType().isCall()) callRefs.add(r);
        }
        pw.println("direct call sites: " + callRefs.size());
        pw.println();

        Function best = null;
        long bestSize = Long.MAX_VALUE;
        Set<Address> bestCallees = null;

        for (Reference r : callRefs) {
            Function caller = fm.getFunctionContaining(r.getFromAddress());
            if (caller == null) continue;
            long sz = caller.getBody().getNumAddresses();
            Set<Address> callees = findCallees(caller, rm, fm);

            StringBuilder calls = new StringBuilder();
            for (Address c : callees) {
                Function cf = fm.getFunctionAt(c);
                long csz = cf.getBody().getNumAddresses();
                long crva = c.getOffset() - 0x400000;
                calls.append(String.format("    -> %s @ %s (rva 0x%x, %d bytes)%n",
                        cf.getName(), c, crva, csz));
            }
            pw.println(String.format("caller: %s @ %s (rva 0x%x, %d bytes, %d callees)",
                    caller.getName(), caller.getEntryPoint(),
                    caller.getEntryPoint().getOffset() - 0x400000, sz, callees.size()));
            pw.print(calls);
            pw.println();

            if (sz < bestSize && sz >= 8 && callees.size() >= 1 && callees.size() <= 4) {
                bestSize = sz;
                best = caller;
                bestCallees = callees;
            }
        }

        if (best != null) {
            pw.println("=== CHOSEN TRANCHE 1 TARGET ===");
            long rva = best.getEntryPoint().getOffset() - 0x400000;
            pw.println("caller: " + best.getName() + " @ " + best.getEntryPoint());
            pw.println("entry RVA: 0x" + Long.toHexString(rva));
            pw.println("size: " + bestSize);
            pw.println("bytes_hex: " + dumpBytes(mem, best.getEntryPoint(), bestSize));
            pw.println();
            for (Address c : bestCallees) {
                Function cf = fm.getFunctionAt(c);
                long cRva = c.getOffset() - 0x400000;
                long cSz = cf.getBody().getNumAddresses();
                pw.println(String.format("callee %s rva=0x%x size=%d bytes=%s",
                        cf.getName(), cRva, cSz,
                        dumpBytes(mem, c, cSz)));
            }
        }
        pw.close();
        println("WROTE " + outDir + "/callers.txt");
        if (best != null) {
            println("BEST_CALLER: " + best.getName() + " @ " + best.getEntryPoint() + " size=" + bestSize);
        } else {
            println("NO_CALLER_FOUND");
        }
    }
}
