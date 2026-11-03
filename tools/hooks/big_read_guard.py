#!/usr/bin/env python3
"""Garde PreToolUse : empeche de bruler le contexte du gros modele sur du volume.

Bloque un Read INTEGRAL d'un fichier > 2000 lignes ou > 500 Ko et renvoie la
commande `tools/astra read` a faire a la place. Un Read avec offset/limit passe
toujours : la lecture ciblee est le bon reflexe, c'est le `cat` complet qui est
interdit. Sortie 2 = bloque et le message va au modele.
"""
import json, os, sys

MAX_LINES, MAX_BYTES = 2000, 500_000

try:
    ev = json.load(sys.stdin)
except Exception:
    sys.exit(0)

if os.environ.get("BO2_ALLOW_BIG_READ") == "1":
    sys.exit(0)

ti = ev.get("tool_input") or {}
path = ti.get("file_path") or ""
if not path or ti.get("offset") or ti.get("limit"):
    sys.exit(0)

try:
    size = os.path.getsize(path)
except OSError:
    sys.exit(0)

if size <= MAX_BYTES:
    # compte les lignes seulement si le fichier est petit : pas de cout inutile
    try:
        with open(path, "rb") as fh:
            lines = sum(1 for _ in fh)
    except OSError:
        sys.exit(0)
    if lines <= MAX_LINES:
        sys.exit(0)
    why = f"{lines} lignes"
else:
    why = f"{size/1e6:.1f} Mo"

print(
    f"BLOQUE — `{path}` fait {why}. Le lire en entier brule des dizaines de\n"
    f"milliers de tokens pour rien (regle CLAUDE.md : > 400 lignes -> deleguer).\n"
    f"\nFais plutot :\n"
    f'  tools/astra read "{path}" "<ta question precise>"\n'
    f"\nOu, si tu sais deja quoi regarder :\n"
    f"  - Read avec offset/limit (toujours autorise)\n"
    f"  - grep -n / sed -n via Bash\n"
    f"\nEchappatoire si tu as vraiment besoin du fichier entier :\n"
    f"  BO2_ALLOW_BIG_READ=1 dans l'environnement.",
    file=sys.stderr,
)
sys.exit(2)
