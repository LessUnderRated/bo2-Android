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
# SHA-256 of the section contents only: the headers differ from one way of removing the wrapper to another (issue #1).
sections_sha(){ python3 -I -c 'import sys,struct,hashlib
b=open(sys.argv[1],"rb").read();p=struct.unpack_from("<I",b,60)[0];n=struct.unpack_from("<H",b,p+6)[0];t=p+24+struct.unpack_from("<H",b,p+20)[0];h=hashlib.sha256()
for i in range(n):
    r,o=struct.unpack_from("<II",b,t+40*i+16);h.update(b[o:o+r])
print(h.hexdigest())' "$1" 2>/dev/null; }
verifie(){  # <image> <expected SHA-256 of its sections>
  [ -f "$S/$1" ] || die "missing $S/$1: put the three Steam exes, SteamStub wrapper removed, in $S/ under these names (README, Installation)"
  head -c 4096 "$S/$1" | grep -q -a '\.bind' && die "$S/$1 still has its SteamStub wrapper: it is the right Steam version, only the wrapper must be removed (README, Install)"
  [ "$(sections_sha "$S/$1")" = "$2" ] || die "$S/$1 is not the Steam build this port translates: use the current Steam version, with only the SteamStub wrapper removed"
  echo "ok: $1"
}
verifie t6sp.exe.unpacked.exe 4a5e30c859422f866d6392f579372c1d8d2fe4e8d782ef56cbb0137f8a66b9d8
verifie t6mp.exe.unpacked.exe 036a53668b26aff9b43546392bb2b17c4f2da933f777145d86c75687fd4e6f26
verifie t6zm.exe.unpacked.exe 08e72c7fd4ceb9eb43104ab6a70231b928859e3081bae0afcd839417ff002f24
echo "$D" > ~/.bo2-native/game_dir && echo "game folder: $D"
