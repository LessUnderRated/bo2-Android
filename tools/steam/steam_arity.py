#!/usr/bin/env python3
"""Arity (stack bytes / 4) of every method of the old Steam interfaces the game asks for, from Valve's public headers.

Source: ValveSoftware/source-sdk-2013, first commit (39ed875, 2013-06-26), mp/src/public/steam/. Those headers carry
exactly the versions the game asks for: SteamUser016, SteamFriends013, STEAMAPPS_INTERFACE_VERSION005,
STEAMUSERSTATS_INTERFACE_VERSION011, SteamNetworking005, SteamGameServer011; Utils is 006 there, which is 005 plus
GetSteamUILanguage at the end, so 005 is its first 23 slots. Licence: Source 1 SDK License (LICENSE at the root of
that repository). Nothing of the headers is copied: they are downloaded to a temporary folder, and clang computes
from them the vtable order (-fdump-vtable-layouts) and the `ret N` of each method (an override of each one compiled
for i686-pc-windows-msvc, the ABI of t6*.exe). Only those numbers are written out.

usage: steam_arity.py [--c]      table (default), or the C table runtime/win32/steam_arity.inc
Needs network once (raw.githubusercontent.com) and llvm (clang++ of llvm@17 or PATH).
Self-check: ISteamApps slot 7 (BIsDlcInstalled) takes 1 argument, ISteamUser slot 2 (GetSteamID) 1 (hidden CSteamID).
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SHA = '39ed87570bdb2f86969d4be821c94b722dc71179'
URL = f'https://raw.githubusercontent.com/ValveSoftware/source-sdk-2013/{SHA}/mp/src/public/steam/'
HEADERS = ['steam_api.h', 'steam_gameserver.h', 'steamtypes.h', 'steamclientpublic.h', 'isteamclient.h',
           'isteamuser.h', 'isteamfriends.h', 'isteamutils.h', 'isteammatchmaking.h', 'matchmakingtypes.h',
           'isteamuserstats.h', 'isteamapps.h', 'isteamnetworking.h', 'isteamremotestorage.h', 'isteamscreenshots.h',
           'isteamhttp.h', 'steamhttpenums.h', 'isteamunifiedmessages.h', 'isteamcontroller.h',
           'isteamgameserver.h', 'isteamgameserverstats.h']
# Runtime id (IF_* of remaining_dlls_impl.cpp) -> interface class, slot count of the version the game asks for
# (None: the whole header version; Utils: 005 = the 23 first slots of 006).
WANT = [(1, 'ISteamUser', None), (2, 'ISteamUtils', 23), (3, 'ISteamUserStats', None), (4, 'ISteamApps', None),
        (5, 'ISteamFriends', None), (6, 'ISteamNetworking', None), (7, 'ISteamGameServer', None)]


def clang():
    for c in ('/opt/homebrew/opt/llvm@17/bin/clang++', '/opt/homebrew/opt/llvm/bin/clang++', shutil.which('clang++')):
        if c and Path(c).exists():
            return c
    sys.exit('clang++ introuvable')


def arities():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / 'steam').mkdir()
        (d / 'inc').mkdir()
        for h in HEADERS:
            subprocess.run(['curl', '-sfL', '--max-time', '30', '-o', str(d / 'steam' / h), URL + h], check=True)
        # The headers include two C headers; no MSVC SDK here, three declarations are enough.
        stub = 'typedef unsigned int size_t;\nextern "C" char* strncpy(char*, const char*, size_t);\n' \
               'extern "C" int Q_snprintf(char*, size_t, const char*, ...);\n'
        for h in ('stdio.h', 'string.h'):
            (d / 'inc' / h).write_text(stub)
        flags = ['-target', 'i686-pc-windows-msvc', '-w', '-nostdinc', '-isystem', str(d / 'inc'), '-I', str(d)]
        inc = '#include "steam/steam_api.h"\n#include "steam/steam_gameserver.h"\n'
        # 1) vtable order, as MSVC lays it out (overloads included). Calling one virtual method per class makes
        # clang compute (and dump) each layout.
        first = {'ISteamUser': 'BLoggedOn()', 'ISteamUtils': 'GetAppID()', 'ISteamUserStats': 'RequestCurrentStats()',
                 'ISteamApps': 'BIsSubscribed()', 'ISteamFriends': 'GetPersonaName()',
                 'ISteamNetworking': 'AllowP2PPacketRelay(false)', 'ISteamGameServer': 'BSecure()'}
        (d / 'v.cpp').write_text(inc + 'void f(' + ', '.join(f'{c}* p{i}' for i, (_, c, _) in enumerate(WANT)) +
                                 ') {' + ''.join(f' p{i}->{first[c]};' for i, (_, c, _) in enumerate(WANT)) + ' }\n')
        out = subprocess.run([clang(), *flags, '-S', '-o', '/dev/null', '-Xclang', '-fdump-vtable-layouts',
                              str(d / 'v.cpp')], capture_output=True, text=True)
        dump = out.stdout + out.stderr
        sig = {}
        for cls in [c for _, c, _ in WANT]:
            m = re.search(rf"^VFTable for '{cls}' \((\d+) entries\)\.\n((?:.*\n)*?)\n", dump, re.M)
            if not m:
                sys.exit(f'pas de vtable pour {cls}:\n{dump[-2000:]}')
            rows = re.findall(rf'^\s*\d+ \| (.*?)\b{cls}::(\w+)\((.*)\) \[pure\]$', m.group(2), re.M)
            sig[cls] = [(r.strip(), n, p) for r, n, p in rows]
        # 2) an override of every method, compiled: the callee's `ret N` is the ABI.
        src = [inc]
        for i, (_, cls, _) in enumerate(WANT):
            src.append(f'struct Impl{i} : {cls} {{')
            for k, (r, n, p) in enumerate(sig[cls]):
                body = '{}' if r == 'void' else f'{{ typedef {r} R; return R(); }}'
                src.append(f'  {r} {n}({p}) override {body}')
            src.append(f'}}; Impl{i} impl{i};')
        (d / 'o.cpp').write_text('\n'.join(src) + '\n')
        asm = subprocess.run([clang(), *flags, '-O0', '-S', '-o', '-', str(d / 'o.cpp')],
                             capture_output=True, text=True)
        if asm.returncode:
            sys.exit(asm.stderr[-3000:])
        a = asm.stdout
        res = {}
        for i, (_, cls, n) in enumerate(WANT):
            # The vftable of Impl<i> lists its methods in slot order (after the RTTI locator).
            vt = re.search(rf'largest,"\?\?_7Impl{i}@@6B@"\n(?:.*\n){{2}}((?:\s+\.long\s+.*\n)+)', a)
            if not vt:
                sys.exit(f'pas de vftable pour Impl{i}')
            fns = [f for f in re.findall(r'\.long\s+"(.*?)"', vt.group(1)) if '??_R4' not in f]
            out = []
            for f in fns[:n]:
                body = re.search(rf'^"{re.escape(f)}":.*\n((?:.*\n)*?)\s+# -- End function', a, re.M)
                if not body:
                    sys.exit(f'pas de corps pour {f}')
                text = body.group(1)
                rets = set(re.findall(r'^\s+retl(?:\s+\$(\d+))?', text, re.M))
                if len(rets) != 1:
                    sys.exit(f'{f}: ret {rets}')
                b = int(rets.pop() or 0)
                out.append(b // 4)
            res[cls] = list(zip(out, [s[1] for s in sig[cls][:n]]))
            if len(res[cls]) != (n or len(sig[cls])):
                sys.exit(f'{cls}: {len(res[cls])} emplacements au lieu de {n}')
        return res


def main():
    res = arities()
    assert res['ISteamApps'][7] == (1, 'BIsDlcInstalled') and res['ISteamUser'][2] == (1, 'GetSteamID'), 'auto-contrôle'
    if '--c' not in sys.argv:
        for _, cls, _ in WANT:
            for k, (a, n) in enumerate(res[cls]):
                print(f'{cls}\t{k}\t{a}\t{n}')
        return
    print(f'// Generated by tools/steam/steam_arity.py --c from Valve\'s public Steamworks headers (source-sdk-2013 {SHA[:7]}):\n'
          '// arguments of each interface slot, for the stack cleanup of thiscall. Do not edit.')
    for _, cls, _ in WANT:
        print(f'static const int8_t kSteamArity{cls[6:]}[] = {{' + ','.join(str(a) for a, _ in res[cls]) + '};')
    print('static const struct { const int8_t* a; int n; } kSteamArity[] = {{nullptr,0},' +
          ','.join(f'{{kSteamArity{c[6:]},{len(res[c])}}}' for _, c, _ in WANT) + '};')


if __name__ == '__main__':
    main()
