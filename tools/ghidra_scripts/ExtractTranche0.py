# ExtractTranche0.py — Ghidra headless postScript (Jython)
# Run via: analyzeHeadless <proj> <projname> -process t6sp.exe -postScript ExtractTranche0.py <out_dir>
#
# Extracts Tranche-0 data from the analyzed program:
#   - total function count
#   - entry point function
#   - per-DLL import counts and xref counts, with focus on renderer DLLs
#     (d3d11, dxgi, d3d9, d3d8) to answer "is D3D11 the real renderer path?"
#   - call sites to D3D11CreateDeviceAndSwapChain / CreateDXGIFactory1
#
# Output: <out_dir>/tranche0.json + human-readable .txt
#@category BO2
#@runtime Jython

import os, sys, json
from ghidra.program.model.listing import Function
from ghidra.program.model.symbol import SymbolType

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ghidra-out"

listing = currentProgram.getListing()
funcMgr = currentProgram.getFunctionManager()
refMgr = currentProgram.getReferenceManager()
symTbl = currentProgram.getSymbolTable()
addrFac = currentProgram.getAddressFactory()

# --- total functions ---
funcs = list(funcMgr.getFunctions(True))
n_funcs = len(funcs)

# --- entry point ---
entry = None
try:
    es = currentProgram.getSymbolTable().getExternalEntryPointIterator()
    # simpler: use the memory block / entry from PE optional header via the "_entry" symbol
    entry_syms = list(symTbl.getSymbols("_entry"))
    if not entry_syms:
        entry_syms = list(symTbl.getSymbols("entry"))
    if entry_syms:
        entry = {"name": entry_syms[0].getName(), "addr": str(entry_syms[0].getAddress())}
except Exception as e:
    entry = {"error": str(e)}

# --- imports: group by DLL, count functions and xrefs ---
# External symbols carry the DLL in their parent namespace.
renderer_dlls = {"d3d11.dll", "d3d10.dll", "d3d9.dll", "d3d8.dll", "dxgi.dll"}
imports = {}      # dll -> {n_funcs, n_xrefs, funcs:[{name, n_xrefs}]}
renderer_calls = []  # call sites to renderer creation funcs

for sym in symTbl.getExternalSymbols():
    name = sym.getName()
    parent = sym.getParentNamespace()
    dll = str(parent).lower()
    if dll not in imports:
        imports[dll] = {"n_funcs": 0, "n_xrefs": 0, "funcs": []}
    rec = imports[dll]
    rec["n_funcs"] += 1
    # count references to this external symbol
    n_xref = 0
    refs = refMgr.getReferencesTo(sym.getAddress())
    call_sites = []
    for r in refs:
        n_xref += 1
        if r.getReferenceType().isCall() or r.getReferenceType().isData():
            call_sites.append(str(r.getFromAddress()))
    rec["n_xrefs"] += n_xref
    rec["funcs"].append({"name": name, "n_xrefs": n_xref})
    # renderer creation calls
    if dll in renderer_dlls and name.lower() in (
        "d3d11createdeviceandswapchain", "d3d11createdevice",
        "createdxgifactory1", "createdxgifactory",
        "direct3dcreate9", "direct3dcreate9ex"):
        renderer_calls.append({
            "dll": dll, "func": name, "n_call_sites": n_xref,
            "call_sites": call_sites[:20],
        })

# sort
for d in imports.values():
    d["funcs"].sort(key=lambda f: -f["n_xrefs"])

out = {
    "binary": str(currentProgram.getExecutablePath()),
    "language": str(currentProgram.getLanguageID()),
    "n_functions": n_funcs,
    "entry": entry,
    "n_import_dlls": len(imports),
    "imports": dict(sorted(imports.items(), key=lambda kv: -kv[1]["n_xrefs"])),
    "renderer_calls": renderer_calls,
}

json_path = os.path.join(OUT_DIR, "tranche0.json")
txt_path = os.path.join(OUT_DIR, "tranche0.txt")
with open(json_path, "w") as f:
    json.dump(out, f, indent=2)

with open(txt_path, "w") as f:
    f.write("=== Tranche 0 extraction: %s ===\n" % out["binary"])
    f.write("Language: %s\n" % out["language"])
    f.write("Functions: %d\n" % out["n_functions"])
    f.write("Entry: %s\n" % entry)
    f.write("Import DLLs: %d\n\n" % out["n_import_dlls"])
    f.write("Top DLLs by xref count:\n")
    for dll, rec in sorted(imports.items(), key=lambda kv: -kv[1]["n_xrefs"])[:12]:
        f.write("  %-18s funcs=%-4d xrefs=%d\n" % (dll, rec["n_funcs"], rec["n_xrefs"]))
    f.write("\nRenderer creation calls:\n")
    if renderer_calls:
        for rc in renderer_calls:
            f.write("  %s!%s  call_sites=%d  %s\n" % (rc["dll"], rc["func"], rc["n_call_sites"], rc["call_sites"][:5]))
    else:
        f.write("  (none found — renderer may be loaded dynamically via LoadLibrary/GetProcAddress)\n")

print("WROTE %s" % json_path)
print("WROTE %s" % txt_path)
print("FUNCTIONS=%d" % n_funcs)
print("IMPORT_DLLS=%d" % len(imports))
print("RENDERER_CALLS=%d" % len(renderer_calls))
