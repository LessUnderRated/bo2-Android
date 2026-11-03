// ExtractTranche0.java — Ghidra headless GhidraScript (Java).
// Run via: analyzeHeadless <proj> <projname> -process t6sp.exe
//          -postScript ExtractTranche0.java <out_dir>
//
// Extracts Tranche-0 data: function count, entry, per-DLL import/xref counts,
// and call sites to renderer-creation functions (d3d11/dxgi/d3d9) to answer
// "is D3D11 the real renderer path, or loaded dynamically?"
//@category BO2

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

public class ExtractTranche0 extends GhidraScript {

    private static final Set<String> RENDERER_DLLS = new HashSet<>(Arrays.asList(
            "d3d11.dll", "d3d10.dll", "d3d9.dll", "d3d8.dll", "dxgi.dll"));
    private static final Set<String> RENDERER_FUNCS = new HashSet<>(Arrays.asList(
            "d3d11createdeviceandswapchain", "d3d11createdevice",
            "createdxgifactory1", "createdxgifactory",
            "direct3dcreate9", "direct3dcreate9ex"));

    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp/ghidra-out";
        new java.io.File(outDir).mkdirs();

        FunctionManager fm = currentProgram.getFunctionManager();
        SymbolTable st = currentProgram.getSymbolTable();
        ReferenceManager rm = currentProgram.getReferenceManager();

        // total functions
        int nFuncs = 0;
        FunctionIterator it = fm.getFunctions(true);
        while (it.hasNext()) { nFuncs++; it.next(); }

        // entry point symbol
        String entry = "(none)";
        SymbolIterator entrySyms = st.getSymbols("_entry");
        if (!entrySyms.hasNext()) entrySyms = st.getSymbols("entry");
        if (entrySyms.hasNext()) {
            Symbol s = entrySyms.next();
            entry = s.getName() + " @ " + s.getAddress();
        }

        // imports grouped by DLL
        Map<String, int[]> dllStats = new HashMap<>();   // dll -> [nFuncs, nXrefs]
        List<String> rendererHits = new ArrayList<>();

        for (Symbol sym : st.getExternalSymbols()) {
            String name = sym.getName();
            String dll = sym.getParentNamespace().getName().toLowerCase();
            int[] stt = dllStats.computeIfAbsent(dll, k -> new int[2]);
            stt[0]++;
            int nx = 0;
            List<String> sites = new ArrayList<>();
            ReferenceIterator refs = rm.getReferencesTo(sym.getAddress());
            while (refs.hasNext()) {
                Reference r = refs.next();
                nx++;
                if (sites.size() < 20 && (r.getReferenceType().isCall() || r.getReferenceType().isData())) {
                    sites.add(r.getFromAddress().toString());
                }
            }
            stt[1] += nx;
            if (RENDERER_DLLS.contains(dll) && RENDERER_FUNCS.contains(name.toLowerCase())) {
                rendererHits.add(String.format("  %s!%s  call_sites=%d  %s",
                        dll, name, nx, sites.size() > 5 ? sites.subList(0,5) : sites));
            }
        }

        // sort DLLs by xref count
        List<Map.Entry<String,int[]>> sorted = new ArrayList<>(dllStats.entrySet());
        sorted.sort((a,b) -> Integer.compare(b.getValue()[1], a.getValue()[1]));

        String txtPath = outDir + "/tranche0.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(txtPath));
        pw.println("=== Tranche 0 extraction: " + currentProgram.getExecutablePath() + " ===");
        pw.println("Language: " + currentProgram.getLanguageID());
        pw.println("Functions: " + nFuncs);
        pw.println("Entry: " + entry);
        pw.println("Import DLLs: " + dllStats.size());
        pw.println();
        pw.println("Top DLLs by xref count:");
        int shown = 0;
        for (Map.Entry<String,int[]> e : sorted) {
            if (shown++ >= 18) break;
            pw.printf("  %-18s funcs=%-4d xrefs=%d%n", e.getKey(), e.getValue()[0], e.getValue()[1]);
        }
        pw.println();
        pw.println("Renderer creation calls (static imports):");
        if (rendererHits.isEmpty()) {
            pw.println("  (none — renderer is likely loaded dynamically via LoadLibrary/GetProcAddress)");
        } else {
            for (String h : rendererHits) pw.println(h);
        }
        pw.close();

        println("WROTE " + txtPath);
        println("FUNCTIONS=" + nFuncs);
        println("IMPORT_DLLS=" + dllStats.size());
        println("RENDERER_CALLS=" + rendererHits.size());
    }
}
