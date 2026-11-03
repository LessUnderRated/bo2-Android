// DumpAllFunctions.java — export all function entries (VA, size, bytes) from
// the analyzed Ghidra project to a CSV file. This is the input for batch lifting.
//
// Usage: analyzeHeadless <proj> <name> -process t6sp.exe -noanalysis \
//   -postScript DumpAllFunctions.java <out_csv>
//@category BO2

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpAllFunctions extends GhidraScript {

    @Override
    public void run() throws Exception {
        String outPath = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp/all_functions.csv";
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();

        PrintWriter pw = new PrintWriter(new FileWriter(outPath));
        pw.println("va,rva,size,bytes_hex,name");

        int count = 0;
        FunctionIterator it = fm.getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            if (f.isExternal()) continue;
            Address entry = f.getEntryPoint();
            long size = f.getBody().getNumAddresses();
            if (size < 4 || size > 100000) continue;  // skip tiny/huge

            long rva = entry.getOffset() - 0x400000;
            StringBuilder hex = new StringBuilder();
            try {
                for (long i = 0; i < size; i++) {
                    byte b = mem.getByte(entry.add(i));
                    hex.append(String.format("%02x", b & 0xff));
                }
            } catch (MemoryAccessException e) {
                continue;  // skip functions with unreadable bytes
            }

            pw.println(entry.getOffset() + ",0x" + Long.toHexString(rva) + "," +
                    size + "," + hex + "," + f.getName());
            count++;
        }
        pw.close();
        println("EXPORTED " + count + " functions to " + outPath);
    }
}
