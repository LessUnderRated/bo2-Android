#!/usr/bin/env python3
"""Arity audit: cross-reference each PE import's *actual* stub pop against the
real stdcall/cdecl calling convention. Reproduces the runtime name-lookup
(including the #N vs Ordinal_N ordinal bug) exactly as win32_shim_init does."""
import json, re, sys, collections

ROOT = "."
FILES = [f"{ROOT}/runtime/win32/kernel32_impl.cpp",
         f"{ROOT}/runtime/win32/user32_impl.cpp",
         f"{ROOT}/runtime/win32/remaining_dlls_impl.cpp"]

# ---- 1. extract impl_func -> (conv, pop_words) --------------------------------
# macros that expand to a full impl with a fixed pop count
FIXED_MACRO = {
 'STUB0':0,'STUB1':1,'STUB2':2,'STUB3':3,'STUB4':4,'STUB5':5,'STUB6':6,'STUB7':7,'STUB8':8,'STUB_RET1_1':1,
 'USTUB0':0,'USTUB1':1,'USTUB2':2,'USTUB3':3,'USTUB4':4,
 'DLL_STUB0':0,'DLL_STUB1':1,'DLL_STUB2':2,'DLL_STUB3':3,'DLL_STUB4':4,
 'DLL_STUB5':5,'DLL_STUB6':6,'DLL_STUB7':7,'DLL_STUB_HANDLE1':1,'DLL_STUB_HANDLE2':2,
 'DLL_STUB_RET1_1':1,'DLL_STUB_RET1_2':2,'DLL_STUB_RET1_3':3,'DLL_STUB_RET1_5':5,
}
HANDLE_MACRO = {'STUB_HANDLE','USTUB_HANDLE'}   # (name, n)
CDECL_MACRO  = {'BINK_STUB'}                     # cdecl, pop 0

impl_pop = {}   # impl_func -> ('stdcall'|'cdecl', pop_words)

src_all = ""
for f in FILES:
    src_all += open(f).read() + "\n"

# fixed-arity macros
for m,p in FIXED_MACRO.items():
    for name in re.findall(rf'\b{m}\((\w+)\)', src_all):
        impl_pop['impl_'+name] = ('stdcall', p)
for m in HANDLE_MACRO:
    for name,n in re.findall(rf'\b{m}\((\w+),\s*(\d+)\)', src_all):
        impl_pop['impl_'+name] = ('stdcall', int(n))
for m in CDECL_MACRO:
    for name in re.findall(rf'\b{m}\((\w+)\)', src_all):
        impl_pop['impl_'+name] = ('cdecl', 0)

# hand-written functions: static void impl_NAME(State& s) { ... }
fn_re = re.compile(r'static void (impl_\w+)\(State&\s*\w+\)\s*\{', re.M)
bodies = {}
for mobj in fn_re.finditer(src_all):
    name = mobj.group(1)
    i = mobj.end(); depth=1; j=i
    while j < len(src_all) and depth:
        c=src_all[j]
        if c=='{':depth+=1
        elif c=='}':depth-=1
        j+=1
    bodies[name] = src_all[i:j-1]

def resolve(name, seen=None):
    if name in impl_pop: return impl_pop[name]
    if seen is None: seen=set()
    if name in seen or name not in bodies: return None
    seen.add(name)
    body = bodies[name]
    if 'win32_cdecl_ret(' in body:
        return ('cdecl',0)
    rets = re.findall(r'win32_stdcall_ret\(\s*\w+\s*,\s*(\d+)\)', body)
    if rets:
        vals=set(int(x) for x in rets)
        # take the arity; if inconsistent, report max (still flags mismatch)
        return ('stdcall', max(vals) if len(vals)==1 else max(vals))
    # delegation: body calls another impl_X(s)
    for callee in re.findall(r'\b(impl_\w+)\(\s*\w+\s*\)', body):
        if callee!=name:
            r=resolve(callee, seen)
            if r: return r
    return None

for name in bodies:
    r=resolve(name)
    if r: impl_pop[name]=r

# ---- 2. registration tables: import-name -> impl_func -------------------------
g_by_name = {}
for tbl in re.findall(r'ImportEntry\s+\w+\[\]\s*=\s*\{(.*?)\};', src_all, re.S):
    for key,fn in re.findall(r'\{\s*"([^"]+)"\s*,\s*(impl_\w+)\s*\}', tbl):
        g_by_name[key]=fn   # later tables overwrite earlier (matches init order)

# ---- 3. PE imports (iat_table naming) ----------------------------------------
imports = json.load(open(f"{ROOT}/bin/iat_map.json"))
def iat_name(e): return e['name'] if e['name'] else f"Ordinal_{e['ordinal']}"

# ---- 4. REAL calling convention + arg count (words) --------------------------
# conv: 's'=stdcall(callee pops argcount), 'c'=cdecl(callee pops 0)
S='s'; C='c'
REAL = {
# KERNEL32
'AddVectoredExceptionHandler':(S,2),'CloseHandle':(S,1),'CompareStringW':(S,6),
'CreateDirectoryA':(S,2),'CreateEventA':(S,4),'CreateFileA':(S,7),'CreateFileMappingA':(S,6),
'CreateFileW':(S,7),'CreateMutexA':(S,3),'CreatePipe':(S,4),'CreateProcessA':(S,10),
'CreateSemaphoreA':(S,4),'CreateSemaphoreW':(S,4),'CreateThread':(S,6),'DebugBreak':(S,0),
'DecodePointer':(S,1),'DeleteCriticalSection':(S,1),'DeleteFileA':(S,1),'DuplicateHandle':(S,7),
'EncodePointer':(S,1),'EnterCriticalSection':(S,1),'EnumSystemLocalesA':(S,2),'ExitProcess':(S,1),
'ExitThread':(S,1),'FileTimeToLocalFileTime':(S,2),'FileTimeToSystemTime':(S,2),'FindClose':(S,1),
'FindFirstFileExA':(S,6),'FindNextFileA':(S,2),'FindResourceW':(S,3),'FlushConsoleInputBuffer':(S,1),
'FlushFileBuffers':(S,1),'FormatMessageA':(S,7),'FreeEnvironmentStringsW':(S,1),'FreeLibrary':(S,1),
'GetACP':(S,0),'GetCPInfo':(S,2),'GetCommandLineA':(S,0),'GetCommandLineW':(S,0),'GetComputerNameA':(S,2),
'GetConsoleCP':(S,0),'GetConsoleMode':(S,2),'GetConsoleWindow':(S,0),'GetCurrentDirectoryA':(S,2),
'GetCurrentDirectoryW':(S,2),'GetCurrentProcess':(S,0),'GetCurrentProcessId':(S,0),'GetCurrentThread':(S,0),
'GetCurrentThreadId':(S,0),'GetDateFormatA':(S,6),'GetDriveTypeA':(S,1),'GetDriveTypeW':(S,1),
'GetEnvironmentStringsW':(S,0),'GetExitCodeProcess':(S,2),'GetFileAttributesA':(S,1),
'GetFileInformationByHandle':(S,2),'GetFileInformationByHandleEx':(S,4),'GetFileSize':(S,2),
'GetFileTime':(S,4),'GetFileType':(S,1),'GetFullPathNameA':(S,4),'GetFullPathNameW':(S,4),
'GetLastError':(S,0),'GetLocaleInfoA':(S,4),'GetLocaleInfoW':(S,4),'GetModuleFileNameA':(S,3),
'GetModuleFileNameW':(S,3),'GetModuleHandleA':(S,1),'GetModuleHandleExA':(S,3),'GetModuleHandleW':(S,1),
'GetOEMCP':(S,0),'GetProcAddress':(S,2),'GetProcessAffinityMask':(S,3),'GetProcessHeap':(S,0),
'GetProcessTimes':(S,5),'GetStartupInfoW':(S,1),'GetStdHandle':(S,1),'GetStringTypeW':(S,4),
'GetSystemDirectoryW':(S,2),'GetSystemInfo':(S,1),'GetSystemTime':(S,1),'GetSystemTimeAsFileTime':(S,1),
'GetThreadPriority':(S,1),'GetTickCount':(S,0),'GetTimeFormatA':(S,6),'GetTimeZoneInformation':(S,1),
'GetUserDefaultLCID':(S,0),'GetVersion':(S,0),'GetVersionExA':(S,1),'GetWindowsDirectoryW':(S,2),
'GlobalLock':(S,1),'GlobalMemoryStatus':(S,1),'GlobalSize':(S,1),'GlobalUnlock':(S,1),
'HeapAlloc':(S,3),'HeapCreate':(S,3),'HeapDestroy':(S,1),'HeapFree':(S,3),'HeapQueryInformation':(S,5),
'HeapReAlloc':(S,4),'HeapSetInformation':(S,4),'HeapSize':(S,3),'InitializeCriticalSection':(S,1),
'InitializeCriticalSectionAndSpinCount':(S,2),'InterlockedCompareExchange':(S,3),'InterlockedDecrement':(S,1),
'InterlockedExchange':(S,2),'InterlockedExchangeAdd':(S,2),'InterlockedIncrement':(S,1),'IsDebuggerPresent':(S,0),
'IsProcessorFeaturePresent':(S,1),'IsValidCodePage':(S,1),'IsValidLocale':(S,2),'LCMapStringW':(S,6),
'LeaveCriticalSection':(S,1),'LoadLibraryA':(S,1),'LoadLibraryW':(S,1),'LoadResource':(S,2),'LockResource':(S,1),
'MapViewOfFile':(S,5),'MoveFileA':(S,2),'MulDiv':(S,3),'MultiByteToWideChar':(S,6),'OpenEventA':(S,3),
'OpenFileById':(S,6),'OpenFileMappingA':(S,3),'OpenProcess':(S,3),'OutputDebugStringA':(S,1),'PeekNamedPipe':(S,6),
'PulseEvent':(S,1),'QueryPerformanceCounter':(S,1),'QueryPerformanceFrequency':(S,1),'RaiseException':(S,4),
'ReadConsoleInputA':(S,4),'ReadFile':(S,5),'ReleaseMutex':(S,1),'ReleaseSemaphore':(S,3),
'RemoveVectoredExceptionHandler':(S,1),'ResetEvent':(S,1),'ResumeThread':(S,1),'RtlUnwind':(S,4),
'SetConsoleCtrlHandler':(S,2),'SetConsoleMode':(S,2),'SetEndOfFile':(S,1),'SetEnvironmentVariableA':(S,2),
'SetEnvironmentVariableW':(S,2),'SetErrorMode':(S,1),'SetEvent':(S,1),'SetFileAttributesA':(S,2),
'SetFilePointer':(S,4),'SetHandleCount':(S,1),'SetLastError':(S,1),'SetPriorityClass':(S,2),
'SetProcessAffinityMask':(S,2),'SetStdHandle':(S,2),'SetThreadExecutionState':(S,1),'SetThreadPriority':(S,2),
'SetUnhandledExceptionFilter':(S,1),'SizeofResource':(S,2),'Sleep':(S,1),'SuspendThread':(S,1),
'TerminateProcess':(S,2),'TlsAlloc':(S,0),'TlsFree':(S,1),'TlsGetValue':(S,1),'TlsSetValue':(S,2),
'TryEnterCriticalSection':(S,1),'UnhandledExceptionFilter':(S,1),'UnmapViewOfFile':(S,1),'VirtualAlloc':(S,4),
'VirtualFree':(S,3),'VirtualQuery':(S,3),'WaitForMultipleObjects':(S,4),'WaitForSingleObject':(S,2),
'WideCharToMultiByte':(S,8),'WriteConsoleW':(S,5),'WriteFile':(S,5),'lstrcmpiW':(S,2),'lstrlenW':(S,1),
# PSAPI
'GetProcessMemoryInfo':(S,3),
# ADVAPI32
'CryptAcquireContextA':(S,5),'CryptGenRandom':(S,3),'CryptReleaseContext':(S,2),'DeregisterEventSource':(S,1),
'InitializeSecurityDescriptor':(S,2),'RegCloseKey':(S,1),'RegOpenKeyExW':(S,5),'RegQueryValueExW':(S,6),
'RegisterEventSourceA':(S,2),'ReportEventA':(S,9),'SetSecurityDescriptorDacl':(S,4),
# GDI32
'BitBlt':(S,9),'CreateCompatibleBitmap':(S,3),'CreateCompatibleDC':(S,1),'CreateDCA':(S,4),'CreateFontA':(S,14),
'CreateSolidBrush':(S,1),'DeleteDC':(S,1),'DeleteObject':(S,1),'GetBitmapBits':(S,3),'GetDeviceCaps':(S,2),
'GetObjectA':(S,3),'SelectObject':(S,2),'SetDeviceGammaRamp':(S,2),
# USER32
'AdjustWindowRect':(S,3),'AdjustWindowRectEx':(S,4),'CallWindowProcA':(S,5),'ChangeDisplaySettingsA':(S,2),
'ClientToScreen':(S,2),'CloseClipboard':(S,0),'CloseWindow':(S,1),'CreateWindowExA':(S,12),'DefWindowProcA':(S,4),
'DeleteMenu':(S,3),'DestroyWindow':(S,1),'DispatchMessageA':(S,1),'EnumDisplaySettingsA':(S,3),
'EnumThreadWindows':(S,3),'GetActiveWindow':(S,0),'GetClassLongA':(S,2),'GetClientRect':(S,2),
'GetClipboardData':(S,1),'GetCursorPos':(S,1),'GetDC':(S,1),'GetDesktopWindow':(S,0),'GetForegroundWindow':(S,0),
'GetMenuItemCount':(S,1),'GetMenuItemID':(S,2),'GetMessageA':(S,4),'GetMonitorInfoA':(S,2),
'GetProcessWindowStation':(S,0),'GetSystemMenu':(S,2),'GetSystemMetrics':(S,1),'GetUserObjectInformationW':(S,5),
'GetWindowLongA':(S,2),'GetWindowRect':(S,2),'GetWindowTextA':(S,3),'IsWindow':(S,1),'LoadCursorA':(S,2),
'LoadIconA':(S,2),'LoadImageA':(S,6),'MapVirtualKeyA':(S,2),'MessageBoxA':(S,4),'MonitorFromWindow':(S,2),
'MoveWindow':(S,6),'OpenClipboard':(S,1),'PeekMessageA':(S,5),'PostMessageA':(S,4),'PostQuitMessage':(S,1),
'RegisterClassA':(S,1),'RegisterClassExA':(S,1),'RegisterWindowMessageA':(S,1),'ReleaseDC':(S,2),
'ScreenToClient':(S,2),'SendMessageA':(S,4),'SetClassLongA':(S,3),'SetCursor':(S,1),'SetCursorPos':(S,2),
'SetFocus':(S,1),'SetWindowLongA':(S,3),'SetWindowPos':(S,7),'SetWindowTextA':(S,2),'ShowCursor':(S,1),
'ShowWindow':(S,2),'TranslateMessage':(S,1),
# WINMM
'mixerClose':(S,1),'mixerGetControlDetailsA':(S,3),'mixerGetDevCapsA':(S,3),'mixerGetLineControlsA':(S,3),
'mixerGetLineInfoA':(S,3),'mixerGetNumDevs':(S,0),'mixerOpen':(S,5),'mixerSetControlDetails':(S,3),
'timeBeginPeriod':(S,1),'timeEndPeriod':(S,1),'timeGetTime':(S,0),'waveInGetNumDevs':(S,0),
# SHELL32 / WTSAPI32 / ole32 / d3d11 / dxgi / faultrep
'SHGetFolderPathA':(S,5),'ShellExecuteA':(S,6),'WTSRegisterSessionNotification':(S,2),
'WTSUnRegisterSessionNotification':(S,1),'CLSIDFromString':(S,2),'CoCreateInstance':(S,5),'CoInitializeEx':(S,2),
'CoUninitialize':(S,0),'StringFromGUID2':(S,3),'D3D11CreateDevice':(S,10),'D3D11CreateDeviceAndSwapChain':(S,12),
'CreateDXGIFactory1':(S,2),'ReportFault':(S,2),
# steam_api (cdecl, callee pops 0)
'SteamAPI_Init':(C,0),'SteamAPI_RegisterCallResult':(C,3),'SteamAPI_RegisterCallback':(C,2),
'SteamAPI_RestartAppIfNecessary':(C,1),'SteamAPI_RunCallbacks':(C,0),'SteamAPI_Shutdown':(C,0),
'SteamAPI_UnregisterCallResult':(C,2),'SteamAPI_UnregisterCallback':(C,1),'SteamApps':(C,0),'SteamFriends':(C,0),
'SteamUser':(C,0),'SteamUserStats':(C,0),'SteamUtils':(C,0),
}
# binkw32 decorated names encode stdcall arg bytes: _Name@N  -> stdcall, N/4 words
for e in imports:
    n=e['name']
    if n and n.startswith('_Bink') and '@' in n:
        REAL[n]=(S, int(n.split('@')[1])//4)

# ordinals: winsock/xinput/dsound are stdcall. map the well-known ones.
WS_ORD={1:('accept',3),2:('bind',3),3:('closesocket',1),4:('connect',3),5:('getpeername',3),
 6:('getsockname',3),7:('getsockopt',5),8:('htonl',1),9:('htons',1),10:('inet_addr',1),
 11:('inet_ntoa',1),12:('ioctlsocket',3),13:('listen',2),14:('ntohl',1),15:('ntohs',1),
 16:('recv',4),17:('recvfrom',6),18:('select',5),19:('send',4),20:('sendto',6),21:('setsockopt',5),
 22:('shutdown',2),23:('socket',3),52:('gethostbyname?',1),57:('gethostname?',2),
 111:('WSAGetLastError',0),112:('WSAStartup?',2),115:('WSAStartup',2),151:('__WSAFDIsSet',2)}
XI_ORD={2:('XInputGetState',2),3:('XInputSetState',2),4:('XInputGetCapabilities',3)}
DS_ORD={6:('DirectSound?ord6',3),11:('DirectSound?ord11',3)}  # uncertain, DirectSound* stdcall >=3

# ---- 5. cross-reference -------------------------------------------------------
rows=[]
for e in imports:
    dll=e['dll']; iname=iat_name(e); ordn=e['ordinal']
    # runtime lookup: named imports bare; ordinals DLL-qualified ("<dll>_dll!Ordinal_N")
    lookup = iname if e['name'] else f"{dll.replace('.','_')}!{iname}"
    impl = g_by_name.get(lookup)
    if impl and impl in impl_pop:
        cur_conv,cur_pop = impl_pop[impl]; reg='yes'
    else:
        cur_conv,cur_pop = ('stdcall',0); reg='NO(default)'   # default stub pops 0
        impl = impl or '(none)'
    # real
    realname=None; note=''
    if iname in REAL:
        rconv,rarg = REAL[iname]; realname=iname
    elif e['name'] is None:
        d = dll.split('.')[0].upper()
        if d.startswith('XINPUT') and ordn in XI_ORD: realname,rarg=XI_ORD[ordn];rconv=S
        elif d.startswith('WSOCK') and ordn in WS_ORD: realname,rarg=WS_ORD[ordn];rconv=S
        elif d.startswith('WS2') and ordn in WS_ORD: realname,rarg=WS_ORD[ordn];rconv=S
        elif d.startswith('DSOUND') and ordn in DS_ORD: realname,rarg=DS_ORD[ordn];rconv=S
        else: rconv,rarg=S,None; note='ordinal unresolved'
    else:
        rconv,rarg=S,None; note='NO REAL SIG'
    correct_pop = 0 if rconv==C else rarg
    mism = (rarg is not None) and (cur_pop != correct_pop)
    rows.append(dict(dll=dll,name=iname,real=realname,rconv=rconv,rarg=rarg,
                     impl=impl,reg=reg,cur=cur_pop,correct=correct_pop,mismatch=mism,note=note))

# noreturn impls: pop never executes (exit/pthread_exit/abort) -> benign
NORETURN={'ExitProcess','ExitThread','RaiseException'}
# functions the MSVCR CRT touches during startup (_mtinit + locale/mbctype +
# __crtGetEnvironmentStrings + pre-main). Used to flag "next wall after TLS".
CRT_INIT={'GetVersion','GetVersionExA','GetACP','GetOEMCP','GetCPInfo','IsValidCodePage',
 'WideCharToMultiByte','MultiByteToWideChar','GetStringTypeW','LCMapStringW','CompareStringW',
 'GetLocaleInfoW','GetLocaleInfoA','IsValidLocale','EnumSystemLocalesA','GetUserDefaultLCID',
 'GetStartupInfoW','GetCommandLineA','GetCommandLineW','HeapAlloc','HeapFree','GetProcessHeap',
 'GetModuleHandleA','GetModuleHandleW','GetProcAddress','GetSystemTimeAsFileTime',
 'GetCurrentThreadId','QueryPerformanceCounter','TlsAlloc','TlsGetValue','TlsSetValue',
 'EncodePointer','DecodePointer','SetUnhandledExceptionFilter','GetSystemInfo',
 'InterlockedCompareExchange','InterlockedExchange','SetHandleCount','GetStdHandle',
 'GetFileType','RtlUnwind','IsProcessorFeaturePresent'}
for r in rows:
    r['benign']= r['name'] in NORETURN
    r['crt']  = r['name'] in CRT_INIT
# ---- 6. report ----------------------------------------------------------------
mm=[r for r in rows if r['mismatch']]
print(f"TOTAL imports: {len(rows)}   MISMATCHES: {len(mm)}   "
      f"(unresolved real-sig: {sum(1 for r in rows if r['rarg'] is None)})\n")
bydll=collections.defaultdict(list)
for r in mm: bydll[r['dll']].append(r)
for dll in sorted(bydll):
    print(f"=== {dll}  ({len(bydll[dll])} mismatches) ===")
    for r in sorted(bydll[dll], key=lambda x:x['name']):
        tag = 'UNREG' if r['reg'].startswith('NO') else 'wrong-arity'
        print(f"  {r['name']:<34} real={str(r['rarg']):>3}({r['rconv']})  "
              f"stub_pops={r['cur']:>2}  need={r['correct']:>2}  [{tag}]  {r['impl']}")
    print()
# also list unresolved (can't verify) so nothing is silently dropped
un=[r for r in rows if r['rarg'] is None]
if un:
    print(f"--- UNRESOLVED real signature ({len(un)}) — not counted as mismatch ---")
    for r in sorted(un,key=lambda x:(x['dll'],x['name'])):
        print(f"  {r['dll']:<15} {r['name']:<20} reg={r['reg']} stub_pops={r['cur']}  {r['note']}")

# ---- 7. markdown report -------------------------------------------------------
UNCERTAIN_DLL={'WSOCK32.dll','WS2_32.dll','DSOUND.dll'}  # ordinal arities approximate
crt = [r for r in mm if r['crt'] and not r['benign']]
benign=[r for r in mm if r['benign']]
out=[]
out.append(f"# Import arity audit — t6sp.exe ({len(rows)} imports, {len(mm)} mismatches)\n")
out.append(f"Auto-generated by tools/audit_arity.py. Convention: stdcall callee pops "
           f"`argcount` words; cdecl callee pops 0. `stub_pops` = what the shim currently "
           f"does; `need` = correct. Mismatch ⇒ ESP desync of `(stub_pops-need)*4` bytes per call.\n")
out.append(f"- **{len(mm)}** mismatches across **{len(set(r['dll'] for r in mm))}** DLLs\n"
           f"- **{len(crt)}** are on the CRT startup path (the next wall after the TLS fix)\n"
           f"- **{len(benign)}** are benign no-return impls (exit/abort): "
           f"{', '.join(r['name'] for r in benign)}\n"
           f"- Ordinal DLLs (WSOCK32/WS2_32/DSOUND) arities are best-effort ordinal→name guesses\n")
out.append("\n## CRT-startup-path mismatches (fix these to get past init)\n")
out.append("| DLL | import | real | stub pops | need | kind | impl |")
out.append("|---|---|--:|--:|--:|---|---|")
for r in sorted(crt,key=lambda x:(x['dll'],x['name'])):
    kind='UNREG' if r['reg'].startswith('NO') else 'wrong-arity'
    out.append(f"| {r['dll']} | `{r['name']}` | {r['rarg']} | {r['cur']} | {r['correct']} | {kind} | `{r['impl']}` |")
out.append("\n## All mismatches by DLL\n")
for dll in sorted(bydll):
    unc=' _(ordinal arities approximate)_' if dll in UNCERTAIN_DLL else ''
    out.append(f"\n### {dll} — {len(bydll[dll])}{unc}\n")
    out.append("| import | real | stub pops | need | kind | impl |")
    out.append("|---|--:|--:|--:|---|---|")
    for r in sorted(bydll[dll],key=lambda x:x['name']):
        kind='UNREG' if r['reg'].startswith('NO') else 'wrong-arity'
        flag=' ⚠️CRT' if (r['crt'] and not r['benign']) else (' (no-return)' if r['benign'] else '')
        out.append(f"| `{r['name']}`{flag} | {r['rarg']} | {r['cur']} | {r['correct']} | {kind} | `{r['impl']}` |")
open(f"{ROOT}/docs/import_arity_audit.md","w").write("\n".join(out)+"\n")
print(f"\n[markdown written to docs/import_arity_audit.md; CRT-path mismatches={len(crt)}]")
