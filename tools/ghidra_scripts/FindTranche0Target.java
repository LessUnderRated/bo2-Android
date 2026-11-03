// FindTranche0Target.java — Ghidra script: find a small function that calls
// a specific imported function directly. Ideal Tranche 0 target: short,
// self-contained, ends in (or contains) a call to a known import — that
// import call is our differential snapshot boundary.
//
// Usage: analyzeHeadless <proj> <name> -process t6sp.exe -noanalysis \
//        -postScript FindTranche0Target.java <out_dir> <target_import>
// e.g. target_import = "GetTickCount" or "QueryPerformanceCounter"
//@category BO2

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

public class FindTranche0Target extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = args.length > 0 ? args[0] : "/tmp/ghidra-out";
        String targetImport = args.length > 1 ? args[1].toLowerCase() : "gettickcount";
        new java.io.File(outDir).mkdirs();

        SymbolTable st = currentProgram.getSymbolTable();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();

        // find the external symbol matching the target import
        Symbol extSym = null;
        for (Symbol s : st.getExternalSymbols()) {
            if (s.getName().toLowerCase().equals(targetImport)) {
                extSym = s;
                break;
            }
        }
        if (extSym == null) {
            println("IMPORT_NOT_FOUND: " + targetImport);
            return;
        }

        // find all call sites to it
        List<Reference> callRefs = new ArrayList<>();
        for (Reference r : rm.getReferencesTo(extSym.getAddress())) {
            if (r.getReferenceType().isCall()) {
                callRefs.add(r);
            }
        }
        println("CALL_SITES_TO_" + targetImport + "=" + callRefs.size());

        // for each call site, find the containing function and its size;
        // pick the smallest one as our Tranche 0 target
        List<String> rows = new ArrayList<>();
        Function best = null;
        long bestSize = Long.MAX_VALUE;
        Address bestCallSite = null;

        for (Reference r : callRefs) {
            Address callSite = r.getFromAddress();
            Function f = fm.getFunctionContaining(callSite);
            if (f == null) continue;
            long sz = f.getBody().getNumAddresses();
            rows.add(String.format("  call_site=%s  in func=%s @ %s  size=%d",
                    callSite, f.getName(), f.getEntryPoint(), sz));
            if (sz < bestSize && sz >= 8) {
                bestSize = sz;
                best = f;
                bestCallSite = callSite;
            }
        }

        String txtPath = outDir + "/tranche0_target.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(txtPath));
        pw.println("=== Tranche 0 target search: import=" + targetImport + " ===");
        pw.println("External symbol: " + extSym.getName() + " @ " + extSym.getAddress());
        pw.println("Call sites: " + callRefs.size());
        pw.println();
        for (String row : rows) pw.println(row);
        pw.println();
        if (best != null) {
            pw.println("=== CHOSEN TARGET (smallest containing function) ===");
            pw.println("function:    " + best.getName() + " @ " + best.getEntryPoint());
            pw.println("entry VA:    " + best.getEntryPoint());
            pw.println("size:        " + bestSize + " bytes");
            pw.println("call site:   " + bestCallSite + " -> " + extSym.getName());
            long rva = best.getEntryPoint().getOffset() - 0x400000;
            pw.println("entry RVA:   " + String.format("0x%x", rva));
            pw.println();
            // dump the function bytes as hex for lifting
            pw.print("bytes_hex: ");
            Address a = best.getEntryPoint();
            StringBuilder hex = new StringBuilder();
            for (long i = 0; i < bestSize; i++) {
                byte b = mem.getByte(a.add(i));
                hex.append(String.format("%02x", b & 0xff));
            }
            pw.println(hex.toString());
        } else {
            pw.println("NO suitable target found.");
        }
        pw.close();

        println("WROTE " + txtPath);
        if (best != null) {
            println("BEST: " + best.getName() + " @ " + best.getEntryPoint() + " size=" + bestSize);
        }
    }
}
