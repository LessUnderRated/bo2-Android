#!/bin/bash
# install_steam.sh <game folder> — links the port to YOUR Steam copy of Black Ops 2. The port translates the code of
# the three Steam exes once their SteamStub wrapper is removed; this repository contains no tool for that. The three
# resulting images must be in ~/.bo2-native/steam/ and are checked by SHA-256. Nothing is downloaded or copied elsewhere.
set -eu
cd "$(dirname "$0")/.."
D="${1:?usage: scripts/install_steam.sh <Call of Duty Black Ops 2 folder>}"
D="$(cd "$D" && pwd)"
S=~/.bo2-native/steam
die(){ echo "[prepare] ❌ $*"; exit 1; }
[ -d "$D/zone/all" ] || die "$D/zone/all not found: this is not a Black Ops 2 folder"
[ -f "$D/t6sp.exe" ] || die "$D/t6sp.exe not found: this is not a Black Ops 2 folder"
verifie(){  # <image> <expected SHA-256>
  [ -f "$S/$1" ] || die "missing $S/$1: put the three Steam exes, SteamStub wrapper removed, in $S/ under these names (README, Installation)"
  head -c 4096 "$S/$1" | grep -q -a '\.bind' && die "$S/$1 still has its SteamStub wrapper: it is the right Steam version, only the wrapper must be removed (README, Install)"
  [ "$(shasum -a 256 "$S/$1" | cut -c1-64)" = "$2" ] || die "$S/$1 is not the Steam build this port translates: use the current Steam version, with only the SteamStub wrapper removed"
  echo "ok: $1"
}
verifie t6sp.exe.unpacked.exe f26a0605723e5869a5d496d109df32530d9703bab18ddc812ba12371d45c0eb2
verifie t6mp.exe.unpacked.exe 38bb52d5428cf86110312eafe89b8b69684fcb666ce5ab3dc451fb801d4e630d
verifie t6zm.exe.unpacked.exe 138757715612aa9883a504686dea2e42c3ed9d149cb40558c1279e177913be80
echo "$D" > ~/.bo2-native/game_dir && echo "game folder: $D"
