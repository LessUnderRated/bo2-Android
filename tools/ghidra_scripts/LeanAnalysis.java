// LeanAnalysis.java — turn OFF the analyzers this project does not need, so a
// 12 MB executable can be catalogued without exhausting the Java heap.
//
// WHY. Analysing t6mp.exe with the stock option set died with
//     ERROR caught exception (StackVariableAnalyzer)
//     java.lang.OutOfMemoryError: Java heap space
// eight times, at 8 GB of heap on a 16 GB machine, and the import failed. Going
// higher is not the answer: half the machine is already committed and the
// remainder has to hold the OS and a game run.
//
// The answer is that WE DO NOT NEED WHAT IS EXPENSIVE. boot.py consumes exactly
// one thing from Ghidra — the function catalogue: entry VA, size, bytes. Stack
// variable recovery, decompiler parameter ID and decompiler switch analysis all
// run the decompiler over every function to produce information the lift never
// reads. They are also, by a wide margin, the heaviest passes.
//
// Run as a PRE-script so the options are set before analysis begins:
//   analyzeHeadless <proj> <name> -import <exe> -preScript LeanAnalysis.java
//@category BO2
import ghidra.app.script.GhidraScript;

public class LeanAnalysis extends GhidraScript {
    // Off: everything that drives the decompiler across the whole image.
    private static final String[] OFF = {
        "Stack",
        "Decompiler Parameter ID",
        "Decompiler Switch Analysis",
        "Call Convention ID",
        "Variadic Function Signature Override",
        "DWARF",
        "Demangler GNU",
        "Create Address Tables - One Time",
        "Shared Return Calls",
    };

    @Override
    public void run() throws Exception {
        if (currentProgram == null) {
            println("LeanAnalysis: no program (run me as a -preScript on import)");
            return;
        }
        for (String name : OFF) {
            try {
                setAnalysisOption(currentProgram, name, "false");
                println("LeanAnalysis: disabled \"" + name + "\"");
            } catch (Exception e) {
                // An option this Ghidra build does not have is not an error —
                // say so rather than failing the whole import over a rename.
                println("LeanAnalysis: no such option \"" + name + "\" (skipped)");
            }
        }
        println("LeanAnalysis: function boundaries and references stay ON — "
                + "that is all boot.py reads.");
    }
}
