#!/usr/bin/env bash
# Auto-controle de tools/agent_lock dans un repertoire temporaire.
set -euo pipefail
L="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/agent_lock"
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
export AGENT_LOCK_FILE="$D/lock" AGENT_NAME=test
ko() { echo "ECHEC : $*" >&2; exit 1; }
"$L" take "premier" >/dev/null           || ko "premier take refuse"
sleep 1
! "$L" take "second" 2>/dev/null         || ko "second take accepte"
grep -q '^raison=premier$' "$D/lock"     || ko "verrou ecrase"
"$L" release >/dev/null
"$L" take "apres release" >/dev/null     || ko "take apres release refuse"
if ! pgrep -x boot >/dev/null; then      # sans boot, un verrou vieux de plus de MAX_S est repris
  AGENT_LOCK_MAX_S=0 "$L" take "perime" >/dev/null 2>&1 || ko "verrou perime non repris"
fi
echo "agent_lock : OK"
