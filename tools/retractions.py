#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regenerate the retraction dataset from git + docs/retractions_classified.json.

    tools/retractions.py            # rewrite docs/RETRACTIONS.md and docs/retractions.csv
    tools/retractions.py --check    # exit 1 if a ❌ commit is missing a classification

Every commit whose subject starts with ❌ is a published claim this project later
measured to be wrong. The subject and date come from git; the claim, the mechanism
of the error and its cost are curated in docs/retractions_classified.json, keyed by
short hash. A new ❌ commit shows up here as UNCLASSIFIED until someone writes its row.

⚠️ git is invoked through subprocess ON PURPOSE: the rtk shell hook truncates
`git log` output to ~50 lines, which silently loses two thirds of this corpus.
"""
import csv, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "docs", "retractions_classified.json")
SEP = "\x01C\x01"

# fine class -> family. The families are the actual research result: six ways a
# measurement can be wrong, ranked below by how often each one has bitten.
FAMILY = {
 "A. the instrument could not answer": [
   "unarmed-instrument","bounded-window","broken-match","inert-instrument",
   "instrument-cannot-read","different-measurement","one-shot-predicate",
   "too-loud-probe","non-idempotent-probe","tool-not-validated","wrong-tool-model"],
 "B. the instrument answered about something else": [
   "wrong-structure","wrong-bounds","wrong-index-space","transposed-struct",
   "misaligned-decode","file-vs-live-disassembly","read-past-arguments",
   "allocated-block-as-node","walker-ignores-the-tag","sample-as-population",
   "mixed-stream","counter-not-content","wrong-attribution","pattern-matched-a-number",
   "tag-read-at-wrong-time","misread-data","misread-sentinel","wrong-conflation",
   "wrong-function","shared-visited-state","invariant-at-wrong-granularity"],
 "C. no control, or one sample": [
   "no-control","n-equals-1","non-determinism","two-samples-not-invariance","matching-counts-not-things",
   "best-vs-median","total-not-delta","last-line-not-the-run","stale-default"],
 "D. my own change produced the symptom": [
   "own-change","own-flag","own-probe-perturbs","duplicate-redirect-pairs"],
 "E. the answer was already in the repo": [
   "already-in-repo","stale-doc","offline-check-skipped","counters-in-the-same-log",
   "normal-behaviour","ordering","zero-without-reading-the-writer",
   "never-counted-the-events"],
 "F. a story published as a result": [
   "plausible-mechanism","correlation-with-a-story","derived-not-measured",
   "untested-standing-explanation","causal-direction","wrong-premise",
   "frequency-as-failure-rate","measure-not-percept","wrong-target"],
 "G. killed cheaply, by design": [
   "killed-by-own-control","refuted-by-control","refuted-by-run","clean-negative",
   "probe-prints-healthy-case","knob-answers-itself","prospective-test-failed",
   "instrument-not-fix","one-defect-two-symptoms"],
}
CLASS2FAM = {c: f for f, cs in FAMILY.items() for c in cs}


def git_retractions():
    raw = subprocess.run(
        ["git", "-C", ROOT, "log", "--pretty=format:" + SEP + "%h|%ad|%s", "--date=short"],
        capture_output=True, text=True, check=True).stdout
    out = []
    for chunk in raw.split(SEP):
        chunk = chunk.strip("\n")
        if not chunk:
            continue
        h, d, s = chunk.split("|", 2)
        if s.lstrip().startswith("❌"):
            out.append({"commit": h, "date": d, "subject": s.strip()})
    out.reverse()
    return out


def load():
    rows = git_retractions()
    curated_list = json.load(open(DATA, encoding="utf-8"))
    curated = {c["commit"]: c for c in curated_list if c.get("commit")}
    used = set()
    for i, r in enumerate(rows, 1):
        c = curated.get(r["commit"], {})
        if c:
            used.add(r["commit"])
        r["id"] = "R%02d" % i
        r["claim"] = c.get("claim", "UNCLASSIFIED")
        r["class"] = c.get("class", "UNCLASSIFIED")
        r["mechanism"] = c.get("mechanism", "UNCLASSIFIED")
        r["cost"] = c.get("cost", "")
        r["family"] = CLASS2FAM.get(r["class"], "UNCLASSIFIED")

    # ── TEMOIN NEGATIF ────────────────────────────────────────────────────────
    # Cette table etait indexee par le HASH du commit, et toute entree curee
    # dont le commit est vide (ou ne porte pas ❌ dans son sujet) etait
    # SILENCIEUSEMENT JETEE. Le 2026-09-18 il y en avait trois — R130 (le dvar
    # cg_drawGun), R131 (l'axe vehicule) et R132 (l'axe arme) — ecrites,
    # commitees, et invisibles dans le corpus qu'elles etaient censees nourrir.
    # C'est la classe d'erreur que ce fichier documente, dans le fichier meme.
    # Regle du depot : toute porte machine doit porter son propre temoin
    # negatif — ici, un compteur d'orphelines qui doit valoir 0.
    orphelines = [c for c in curated_list if c.get("commit") not in used]
    for c in orphelines:
        rows.append({
            "commit": c.get("commit") or "(hors ❌)",
            "date": c.get("date", "") or "0000-00-00",
            "subject": c.get("claim", "")[:80],
            "id": c.get("id") or ("R%02d" % (len(rows) + 1)),
            "claim": c.get("claim", "UNCLASSIFIED"),
            "class": c.get("class", "UNCLASSIFIED"),
            "mechanism": c.get("mechanism", "UNCLASSIFIED"),
            "cost": c.get("cost", ""),
            "family": CLASS2FAM.get(c.get("class"), "UNCLASSIFIED"),
        })
    if orphelines:
        print("⚠️  %d entree(s) curee(s) sans commit ❌ correspondant — ajoutee(s) "
              "a la fin au lieu d'etre jetee(s) :" % len(orphelines))
        for c in orphelines:
            print("     %-5s %s" % (c.get("id") or "R??", c.get("claim", "")[:88]))
    else:
        print("temoin : 0 entree curee orpheline (elles sont toutes rattachees)")
    return rows


def write_csv(rows, path):
    cols = ["id", "commit", "date", "family", "class", "claim", "mechanism", "cost", "subject"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({k: r[k] for k in cols})


def write_md(rows, path):
    fams = {}
    for r in rows:
        fams.setdefault(r["family"], []).append(r)
    L = []
    A = L.append
    A("# The retraction corpus\n")
    # ⚠️ LA PLAGE DE DATES SE CALCULE SUR LES LIGNES DATEES, PAS SUR LES BOUTS DE
    # LA LISTE. Les entrees curees sans commit ❌ sont ajoutees A LA FIN avec la
    # date bouchon "0000-00-00" : `rows[-1]["date"]` publiait donc, en tete d'un
    # document genere, « 2026-07-30 to 0000-00-00 » — une plage impossible, dans
    # le fichier meme qui recense les instruments qui mentent. On ne fabrique pas
    # une date de remplacement (ce serait inventer la donnee) : on prend le min et
    # le max des dates REELLES, et on dit combien de lignes n'en ont pas.
    datees = sorted(r["date"] for r in rows if r["date"] and r["date"] != "0000-00-00")
    sans_date = len(rows) - len(datees)
    plage = ("%s to %s" % (datees[0], datees[-1])) if datees else "aucune ligne datee"
    A("**%d published claims this project later measured to be wrong**, %s, "
      "out of %s commits.%s Generated by `tools/retractions.py` from the git log plus "
      "`docs/retractions_classified.json`; re-run it after any new ❌ commit.\n"
      % (len(rows), plage,
         subprocess.run(["git", "-C", ROOT, "rev-list", "--count", "HEAD"],
                        capture_output=True, text=True).stdout.strip(),
         (" %d entry(ies) carry no date yet (curated, not committed as ❌)." % sans_date)
         if sans_date else " Every entry is dated."))
    A("A ❌ commit is not a bug. It is a claim that was written down, sometimes shipped "
      "into the README or the GUIDE, and then killed by a measurement. The value of the "
      "corpus is the *mechanism* column: the same handful of ways of being wrong recur, "
      "and they recur **after being documented**.\n")
    A("## How often each family bites\n")
    A("| family | n | share |")
    A("|---|---:|---:|")
    for f in sorted(fams, key=lambda k: -len(fams[k])):
        A("| %s | %d | %d%% |" % (f, len(fams[f]), round(100.0 * len(fams[f]) / len(rows))))
    A("")
    A("## Every retraction, in order\n")
    for f in sorted(fams, key=lambda k: -len(fams[k])):
        A("### %s — %d\n" % (f, len(fams[f])))
        for r in fams[f]:
            A("**%s · `%s` · %s · %s**  " % (r["id"], r["commit"], r["date"], r["class"]))
            A("> %s\n" % r["claim"])
            A("%s  " % r["mechanism"])
            A("*Cost: %s*\n" % (r["cost"] or "not stated"))
    open(path, "w", encoding="utf-8").write("\n".join(L))


def main():
    rows = load()
    missing = [r for r in rows if r["class"] == "UNCLASSIFIED"]
    if "--check" in sys.argv:
        for r in missing:
            print("UNCLASSIFIED %s %s %s" % (r["commit"], r["date"], r["subject"][:70]))
        print("%d/%d classified" % (len(rows) - len(missing), len(rows)))
        return 1 if missing else 0
    write_csv(rows, os.path.join(ROOT, "docs", "retractions.csv"))
    write_md(rows, os.path.join(ROOT, "docs", "RETRACTIONS.md"))
    print("%d retractions, %d unclassified -> docs/RETRACTIONS.md, docs/retractions.csv"
          % (len(rows), len(missing)))
    for r in missing:
        print("  UNCLASSIFIED %s %s" % (r["commit"], r["subject"][:70]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
