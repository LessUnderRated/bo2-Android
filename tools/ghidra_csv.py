"""ghidra_csv.py — the Ghidra catalogue of each Steam exe (bin/ghidra-out-<target>/, read by boot.py and lift_roots.py) without game bytes.

    BO2_TARGET=sps python3 tools/ghidra_csv.py vider          # "empty", repository: bin/ghidra-out-sps/*.csv -> tools/ghidra-sps/*.csv, bytes_hex cleared
    BO2_TARGET=sps python3 tools/ghidra_csv.py remplir <exe>  # "fill", fresh machine: tools/ghidra-sps/*.csv + exe -> bin/ghidra-out-sps/*.csv
    (same for mps and zms)

`remplir` reads each body back from the exe at its rva and size: on the exe checked by SHA-256, the output is
byte-for-byte identical to the original catalogue (check: `remplir`, then `cmp` against a copy).
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CIBLE = os.environ.get("BO2_TARGET", "sps")
SUFFIXE = "-" + CIBLE
SRC, DST = os.path.join(ROOT, "bin", "ghidra-out" + SUFFIXE), os.path.join(ROOT, "tools", "ghidra" + SUFFIXE)
FICHIERS = ("all_functions.csv", "calltree.csv")


def recopier(de, vers, octets):
    os.makedirs(vers, exist_ok=True)
    for nom in FICHIERS:
        with open(os.path.join(de, nom), newline="") as f_in, \
                open(os.path.join(vers, nom), "w", newline="") as f_out:
            r = csv.DictReader(f_in)
            w = csv.DictWriter(f_out, fieldnames=r.fieldnames, lineterminator="\n")
            w.writeheader()
            for row in r:
                row["bytes_hex"] = octets(int(row["rva"], 16), int(row["size"]))
                w.writerow(row)


def main():
    if sys.argv[1:2] == ["vider"]:
        recopier(SRC, DST, lambda rva, n: "")
    elif sys.argv[1:2] == ["remplir"] and len(sys.argv) == 3:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from pe_parser import PEFile
        pe = PEFile(sys.argv[2]).parse()

        def octets(rva, n):
            off = pe.rva_to_offset(rva)
            return pe._raw[off:off + n].hex()
        recopier(DST, SRC, octets)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
