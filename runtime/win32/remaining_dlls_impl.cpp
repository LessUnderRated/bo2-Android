// remaining_dlls_impl.cpp — stub implementations for the remaining 16 DLLs.
//
// These are all stubbed (return 0 or fake handle) with correct stdcall arg
// counts so the stack stays aligned. Real implementations added as needed:
//   - d3d11/dxgi: handled by the D3D11→Metal layer (not import dispatch)
//   - XINPUT1_3: → GameController framework
//   - DSOUND/WINMM: → CoreAudio/AVAudioEngine
//   - binkw32: → ffmpeg decode of .bik assets
//   - steam_api: → the player's Steam (libsteam_api.dylib), see steam_reel()
//   - WSOCK32/WS2_32: → BSD sockets (if online ever matters)
//   - GDI32/ADVAPI32/ole32/SHELL32/etc: → mostly stubbable
#include "win32_shim.h"
#include "bo2_getenv.h"
#include "guest_monitor.h"
#include "diag_counter.h"      // P661/C15
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>              // isalnum, for the MSL texture-name scan
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>               // g_d3d_mu, the D3D11 device lock (P651)
#include <sys/stat.h>          // mkdir, for BO2_MSL_DUMP

extern "C" {
    uint32_t win32_mem_read32(uint32_t a);
    void win32_mem_write32(uint32_t a, uint32_t v);
    void win32_mem_write16(uint32_t a, uint16_t v);
}
// 🔴 P628/E8 — atomique depuis le 2026-09-21 : c'est un global, incremente
// aussi depuis des fils HOTES (voir sa definition dans kernel32_impl.cpp).
#include <atomic>
#include <arm_neon.h>          // C35, C46 ib_min_max
extern std::atomic<uint32_t> t_fake_handle_counter;

// Generic stubs with specific arg counts
#define DLL_STUB0(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 0); }
#define DLL_STUB1(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 1); }
#define DLL_STUB2(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 2); }
#define DLL_STUB3(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 3); }
#define DLL_STUB4(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 4); }
#define DLL_STUB5(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 5); }
#define DLL_STUB6(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 6); }
#define DLL_STUB7(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 7); }
#define DLL_STUB9(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 9); }
#define DLL_STUB14(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 14); }
#define DLL_STUB_HANDLE1(name) static void impl_##name(State& s) { s.gpr.rax.dword = ++t_fake_handle_counter; win32_stdcall_ret(s, 1); }
#define DLL_STUB_HANDLE2(name) static void impl_##name(State& s) { s.gpr.rax.dword = ++t_fake_handle_counter; win32_stdcall_ret(s, 2); }
#define DLL_STUB_RET1_1(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 1; win32_stdcall_ret(s, 1); }
#define DLL_STUB_RET1_2(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 1; win32_stdcall_ret(s, 2); }
#define DLL_STUB_RET1_3(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 1; win32_stdcall_ret(s, 3); }
#define DLL_STUB_RET1_5(name)  static void impl_##name(State& s) { s.gpr.rax.dword = 1; win32_stdcall_ret(s, 5); }

// --- GDI32 (13 functions) ---
DLL_STUB1(DeleteDC)                 // (hdc)
DLL_STUB1(DeleteObject)
DLL_STUB3(GetBitmapBits)
DLL_STUB9(BitBlt)                   // 9 args
DLL_STUB3(GetObjectA)
DLL_STUB2(SelectObject)             // (hdc, hgdiobj)
DLL_STUB1(CreateSolidBrush)
DLL_STUB3(CreateCompatibleBitmap)
DLL_STUB1(CreateCompatibleDC)
DLL_STUB4(CreateDCA)                // (driver, device, port, initData)
DLL_STUB2(GetDeviceCaps)
DLL_STUB14(CreateFontA)             // 14 args
DLL_STUB2(SetDeviceGammaRamp)

// --- ADVAPI32 (11 functions) ---
DLL_STUB4(SetSecurityDescriptorDacl)      // real arity 4 (was DLL_STUB3 -> leaked 4B into WinMain's frame)
DLL_STUB2(InitializeSecurityDescriptor)   // real arity 2 (was DLL_STUB1 -> leaked 4B into WinMain's frame)
DLL_STUB1(RegCloseKey)              // (hKey)
DLL_STUB2(RegisterEventSourceA)     // (lpUNCServerName, lpSourceName)
DLL_STUB9(ReportEventA)             // 9 args
DLL_STUB1(DeregisterEventSource)
DLL_STUB2(CryptReleaseContext)
// Z-NATIF-14o : Demonware tire l'identifiant (8 octets) et la cle (16) de chaque session par
// CryptGenRandom([0x33e7824], n, tampon) (0xa4b380, retour ignore). Les talons n'ecrivaient rien : identifiant nul
// a chaque session, bdSecurityKeyMap::registerKey (0xa4b9d0) refuse le second et le jeu affiche
// EXE_ERROR_CREATING_SESSION_NONMATCH. Windows : fournisseur non nul, tampon rempli, retour TRUE.
static void impl_CryptAcquireContextA(State& s) {   // (phProv, container, provider, type, flags)
    if (const uint32_t out = win32_arg(s, 0)) win32_mem_write32(out, 0x43525031);   // poignee opaque, jamais dereferencee
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 5);
}
static void impl_CryptGenRandom(State& s) {   // (hProv, dwLen, pbBuffer)
    const uint32_t len = win32_arg(s, 1), buf = win32_arg(s, 2);
    static DiagCounter<4> vus;
    if (vus.bump(win32_mem_read32(s.gpr.rsp.dword)) == 0)
        fprintf(stderr, "[CRYPT] CryptGenRandom(%u octets) ret=0x%08x -> rempli\n", len, win32_mem_read32(s.gpr.rsp.dword));
    for (uint32_t i = 0; i < len; i++) win32_mem_write8(buf + i, (uint8_t)arc4random());
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 3);
}
DLL_STUB6(RegQueryValueExW)         // (hKey, name, rsvd, type, data, cbData)
DLL_STUB5(RegOpenKeyExW)            // (hKey, subKey, options, samDesired, phkResult)

// --- WINMM (12 functions, 6 by ordinal) ---
// time functions: → DeterministicClock
DLL_STUB1(timeBeginPeriod)
DLL_STUB1(timeEndPeriod)
// timeGetTime MUST advance: Sys_Milliseconds (0x61f1b0) returns timeGetTime()-base,
// and DB_FindXAssetHeader's wait loop (0x4637c0) reads elapsed==0 as "wait not
// started" forever -> tid4 (render thread) spun infinitely on streaming_temp_image_N
// during R_Init instead of taking the DB-idle give-up exit (0x46390a->0x4639a8).
extern "C" uint64_t bo2_mono_ns(void);   // kernel32_impl.cpp, honours BO2_CLOCK_SCALE
static void impl_timeGetTime(State& s) {
    s.gpr.rax.dword = (uint32_t)(bo2_mono_ns() / 1000000ull);
    win32_stdcall_ret(s, 0);
}
// mixer/wave: stub
DLL_STUB3(mixerGetLineControlsA)    // (hmxobj, pmxlc, fdwControls)
DLL_STUB5(mixerOpen)                // (phmx, uMxId, dwCallback, dwInstance, fdwOpen)
DLL_STUB3(mixerSetControlDetails)   // (hmxobj, pmxcd, fdwDetails)
DLL_STUB3(mixerGetDevCapsA)         // (uMxId, pmxcaps, cbmxcaps)
DLL_STUB0(waveInGetNumDevs)
DLL_STUB0(mixerGetNumDevs)
DLL_STUB1(mixerClose)
DLL_STUB3(mixerGetControlDetailsA)  // (hmxobj, pmxcd, fdwDetails)
DLL_STUB3(mixerGetLineInfoA)        // (hmxobj, pmxl, fdwInfo)

// --- WSOCK32 (18 functions, all by ordinal) ---
// Winsock 1.1 — stub for offline. Ordinals #2,3,9,10,12,16,17,18,19,20,21,22,23,52,57,111,115,151
// We can't easily map ordinals to names, so use generic stubs
// The IAT table stores ordinal numbers as "#N" — we'll handle them in init
DLL_STUB3(wsock_ordinal_12)  // ioctlsocket(s, cmd, argp)
DLL_STUB5(wsock_ordinal_21)  // setsockopt(s, level, optname, optval, optlen)
DLL_STUB2(wsock_ordinal_22)  // shutdown(s, how)
DLL_STUB2(wsock_ordinal_115) // WSAStartup(wVersionRequested, lpWSAData)
// t6zm: these twelve and connect (4) answer as winsock does, and the title's Demonware client finds a responder
// behind them: runtime/win32/dw_local.cpp (Z-NATIF-09s). bind (2) and gethostname (57) too since Z-NATIF-12d.
void impl_wsock_ordinal_2(State&);   void impl_wsock_ordinal_57(State&);
void impl_wsock_ordinal_3(State&);   void impl_wsock_ordinal_4(State&);   void impl_wsock_ordinal_9(State&);
void impl_wsock_ordinal_10(State&);  void impl_wsock_ordinal_16(State&);  void impl_wsock_ordinal_17(State&);
void impl_wsock_ordinal_18(State&);  void impl_wsock_ordinal_19(State&);  void impl_wsock_ordinal_20(State&);
void impl_wsock_ordinal_23(State&);  void impl_wsock_ordinal_52(State&);  void impl_wsock_ordinal_111(State&);
void impl_wsock_ordinal_151(State&);
// t6zm imports six more (t6sp does not): 15 was called 4 times unanswered, the stack was 16 bytes off
// (run of 08/10). Byte order is a pure function: the real value. 1 (accept) stays unanswered
// until called: its result is a decision, not an arity.
static void impl_wsock_ordinal_8(State& s)  { s.gpr.rax.dword = __builtin_bswap32(win32_arg(s, 0)); win32_stdcall_ret(s, 1); }             // htonl(hostlong)
static void impl_wsock_ordinal_14(State& s) { s.gpr.rax.dword = __builtin_bswap32(win32_arg(s, 0)); win32_stdcall_ret(s, 1); }             // ntohl(netlong)
static void impl_wsock_ordinal_15(State& s) { s.gpr.rax.dword = __builtin_bswap16((uint16_t)win32_arg(s, 0)); win32_stdcall_ret(s, 1); }   // ntohs(netshort)
DLL_STUB0(wsock_ordinal_116) // WSACleanup(void)

// --- WS2_32 (5 functions, by ordinal) ---
DLL_STUB3(ws2_ordinal_4)     // connect(s, name, namelen)
DLL_STUB3(ws2_ordinal_6)     // getsockname(s, name, namelen)
void impl_ws2_ordinal_12(State&);   // inet_ntoa(in), ONE argument: dw_local.cpp (Z-NATIF-12d)
DLL_STUB1(ws2_ordinal_15)    // ntohs(netshort)
DLL_STUB1(ws2_ordinal_112)   // WSASetLastError(iError)

// --- binkw32 (19 functions) ---
// All return 0 — video is decoded natively instead.
//
// 🎯 THESE ARE STDCALL AND THEY MUST POP THEIR ARGUMENTS. The `@N` in
// `_BinkOpen@8` is the MSVC __stdcall decoration: N BYTES of arguments that the
// CALLEE removes. Using win32_cdecl_ret (which pops only the return address)
// leaks N bytes of GUEST STACK on every single call.
//
// The arity flag that used to live here said exactly this and deferred it —
// "Bink is never called during CRT init; switch to a stdcall pop of @N/4 when
// the video path is implemented and we can actually exercise these". It is
// exercised, and the leak is what stops the campaign from loading:
//
//   * the campaign zone load drives a video-open retry loop on tid 2, the
//     STREAMING worker (measured: BinkOpen call #873,000 and climbing);
//   * each turn of that loop calls BinkSetMemory@8 + BinkSetIOSize@4 +
//     BinkSetSoundTrack@8 + BinkOpen@8 = 8+4+8+8 = **28 bytes leaked**;
//   * measured ESP between successive iterations: 0x413a3300 -> 0x4139c5a0 ->
//     0x41395840, i.e. 0x6d60 per 1000 iterations = **28 bytes each**, and the
//     repeating 28-byte record found smeared across 6 MB of that thread's stack
//     is those leaked argument frames;
//   * after ~6 MB it walks out of the thread's 8 MB stack and over the game's
//     lock-free job-node pool, whose head/tail then hold Bink return addresses
//     (0x0070321e, 0x00702fe3). The dequeue at 0x8fc330 live-locks on the
//     corrupted head, the fastfile decompress job is never run, 0x453a80 spins
//     in Sleep(0) for ever, and afghanistan.ff stops after 4 blocks of 3,575.
//
// See docs/handoff/SP-01-campaign-play-state.md for the measurement chain.
// The byte count is carried in each stub's own name (_at_8 -> 8 bytes).
#define BINK_STUB_N(name, bytes) \
    static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, (bytes) / 4); }
#define BINK_STUB(name) BINK_STUB_N(name, 0)
BINK_STUB_N(_BinkSetSoundTrack_at_8, 8)
BINK_STUB_N(_BinkDoFrame_at_4, 4)
BINK_STUB_N(_BinkClose_at_4, 4)
BINK_STUB_N(_BinkStartAsyncThread_at_8, 8)
BINK_STUB_N(_BinkGetError_at_0, 0)
BINK_STUB_N(_BinkOpen_at_8, 8)
BINK_STUB_N(_BinkWaitStopAsyncThread_at_4, 4)
BINK_STUB_N(_BinkGetRealtime_at_12, 12)
BINK_STUB_N(_BinkSetMemory_at_8, 8)
BINK_STUB_N(_BinkDoFrameAsyncWait_at_8, 8)
BINK_STUB_N(_BinkGetRects_at_8, 8)
BINK_STUB_N(_BinkRequestStopAsyncThread_at_4, 4)
BINK_STUB_N(_BinkRegisterFrameBuffers_at_8, 8)
BINK_STUB_N(_BinkNextFrame_at_4, 4)
BINK_STUB_N(_BinkGetFrameBuffersInfo_at_8, 8)
BINK_STUB_N(_BinkPause_at_8, 8)
BINK_STUB_N(_BinkControlBackgroundIO_at_8, 8)
BINK_STUB_N(_BinkDoFrameAsync_at_12, 12)
BINK_STUB_N(_BinkSetIOSize_at_4, 4)

// --- steam_api (13 functions, cdecl) ---
// On demande au Steam Mac du joueur, par SA
// libsteam_api.dylib (livrée dans Steam.app, jamais redistribuée), qui est connecté et s'il possède ce jeu. Steam refuse
// SteamAPI_Init pour un jeu absent de la bibliothèque (« ConnectToGlobalUser failed », essai du 26/10) : alors on ne lance
// pas. Le compte, l'abonnement et les DLC répondus au jeu sont ceux de Steam (steam_vtbl_dispatch), plus rien d'inventé.
#include <dlfcn.h>
#include <unistd.h>
struct SteamReel {
    uint64_t id = 0;
    void* apps = nullptr;
    void* user = nullptr;
    bool (*logged)(void*) = nullptr;           // ISteamUser::BLoggedOn
    void* stats = nullptr;
    bool (*store)(void*) = nullptr;            // ISteamUserStats::StoreStats
    bool (*sub)(void*, uint32_t) = nullptr;   // ISteamApps::BIsSubscribedApp
    bool (*dlc)(void*, uint32_t) = nullptr;   // ISteamApps::BIsDlcInstalled
};
#if defined(BO2_TARGET_MPS)
static constexpr uint32_t kSteamAppId = 202990;   // Black Ops II - Multiplayer
#elif defined(BO2_TARGET_ZMS)
static constexpr uint32_t kSteamAppId = 212910;   // Black Ops II - Zombies
#else
static constexpr uint32_t kSteamAppId = 202970;   // Black Ops II (campagne)
#endif
[[noreturn]] static void steam_reel_refus(const std::string& why) {
    fprintf(stderr, "[STEAM-REEL] refus : %s\n", why.c_str());
    std::string m;   // ni ' ni " dans la commande du shell
    for (char c : why) m += c == '\'' ? std::string("’") : c == '"' ? std::string("”") : std::string(1, c);
    const std::string cmd = "osascript -e 'display alert \"Black Ops II\" message \"" + m + "\" giving up after 600' >/dev/null 2>&1 &";   // détachée : on sort tout de suite
    system(cmd.c_str());
    _exit(78);
}
// Refusal text in the Mac's language (the 9 of tools/setup_window.swift), English otherwise. %u = the Steam app id.
enum SteamRefus { kSteamAbsent, kSteamAncien, kSteamInit, kSteamPasPossede };
static std::string steam_texte(SteamRefus k) {
    static const char* const langues[] = {"fr", "en", "de", "es", "it", "pt", "pl", "ru", "ja"};
    static const char* const textes[][4] = {
        {"Steam pour Mac est introuvable. Installez-le (store.steampowered.com), connectez-vous, puis relancez.",
         "Cette version de Steam pour Mac est trop ancienne : mettez-la à jour.",
         "Steam doit être ouvert et connecté, et Black Ops II (%u) dans votre bibliothèque.",
         "Black Ops II (%u) n’est pas dans votre bibliothèque Steam."},
        {"Steam for Mac was not found. Install it (store.steampowered.com), sign in, then launch again.",
         "This version of Steam for Mac is too old: please update it.",
         "Steam must be open and signed in, with Black Ops II (%u) in your library.",
         "Black Ops II (%u) is not in your Steam library."},
        {"Steam für Mac wurde nicht gefunden. Installieren Sie es (store.steampowered.com), melden Sie sich an und starten Sie erneut.",
         "Diese Version von Steam für Mac ist zu alt: Bitte aktualisieren Sie sie.",
         "Steam muss geöffnet und angemeldet sein, und Black Ops II (%u) muss in Ihrer Bibliothek sein.",
         "Black Ops II (%u) ist nicht in Ihrer Steam-Bibliothek."},
        {"No se encontró Steam para Mac. Instálelo (store.steampowered.com), inicie sesión y vuelva a abrir el juego.",
         "Esta versión de Steam para Mac es demasiado antigua: actualícela.",
         "Steam debe estar abierto y con la sesión iniciada, y Black Ops II (%u) en su biblioteca.",
         "Black Ops II (%u) no está en su biblioteca de Steam."},
        {"Steam per Mac non è stato trovato. Lo installi (store.steampowered.com), acceda e riavvii il gioco.",
         "Questa versione di Steam per Mac è troppo vecchia: la aggiorni.",
         "Steam deve essere aperto e connesso, con Black Ops II (%u) nella Sua libreria.",
         "Black Ops II (%u) non è nella Sua libreria Steam."},
        {"O Steam para Mac não foi encontrado. Instale-o (store.steampowered.com), entre na sua conta e abra o jogo de novo.",
         "Esta versão do Steam para Mac é antiga demais: atualize-a.",
         "O Steam precisa estar aberto e conectado, com o Black Ops II (%u) na sua biblioteca.",
         "O Black Ops II (%u) não está na sua biblioteca Steam."},
        {"Nie znaleziono Steam dla Maca. Należy go zainstalować (store.steampowered.com), zalogować się i uruchomić grę ponownie.",
         "Ta wersja Steam dla Maca jest zbyt stara: należy ją zaktualizować.",
         "Steam musi być otwarty i zalogowany, a Black Ops II (%u) musi być w bibliotece.",
         "Black Ops II (%u) nie ma w bibliotece Steam."},
        {"Steam для Mac не найден. Установите его (store.steampowered.com), войдите в аккаунт и запустите игру снова.",
         "Эта версия Steam для Mac слишком старая: обновите её.",
         "Steam должен быть открыт, вход выполнен, а Black Ops II (%u) — в вашей библиотеке.",
         "Black Ops II (%u) нет в вашей библиотеке Steam."},
        {"Mac版Steamが見つかりません。インストール（store.steampowered.com）してサインインし、もう一度起動してください。",
         "このMac版Steamは古すぎます。アップデートしてください。",
         "Steamを起動してサインインし、Black Ops II（%u）がライブラリにある必要があります。",
         "Black Ops II（%u）がSteamライブラリにありません。"},
    };
    std::string l = "en";   // first of AppleLanguages: ( "fr-FR", ... ) or ( fr, ... )
    if (FILE* p = popen("defaults read -g AppleLanguages 2>/dev/null", "r")) {
        char b[256] = {0};
        fread(b, 1, sizeof b - 1, p);
        pclose(p);
        if (const char* q = strchr(b, '(')) {
            while (*q && !isalpha((unsigned char)*q)) q++;
            if (isalpha((unsigned char)q[0]) && isalpha((unsigned char)q[1])) l.assign(q, 2);
        }
    }
    size_t i = 1;
    for (size_t j = 0; j < sizeof langues / sizeof *langues; j++) if (l == langues[j]) i = j;
    char m[512];
    snprintf(m, sizeof m, textes[i][k], kSteamAppId);
    return m;
}
static const SteamReel& steam_reel() {
    static const SteamReel r = [] {
        SteamReel r;
        const std::string lib = std::string(getenv("HOME")) + "/Library/Application Support/Steam/Steam.AppBundle/Steam/"
                                "Contents/MacOS/Frameworks/Steam Helper.app/Contents/MacOS/libsteam_api.dylib";
        void* h = dlopen(lib.c_str(), RTLD_NOW);
        if (!h) steam_reel_refus(steam_texte(kSteamAbsent));
        auto init = (int (*)(char*))dlsym(h, "SteamAPI_InitFlat");
        // A Steam update (the beta first) can move an interface to a newer version and drop the old accessor (issue #6).
        // The flat ISteam* functions of a libsteam_api follow its newest interface, so take the newest accessor it has.
        auto acces = [h](const char* base, int de) -> void* (*)() {
            for (int v = de + 20; v >= de; v--) {
                char n[64];
                snprintf(n, sizeof n, "%s%03d", base, v);
                if (void* f = dlsym(h, n)) return (void* (*)())f;
            }
            return nullptr;
        };
        auto user = acces("SteamAPI_SteamUser_v", 23);
        auto apps = acces("SteamAPI_SteamApps_v", 9);
        auto sid = (uint64_t (*)(void*))dlsym(h, "SteamAPI_ISteamUser_GetSteamID");
        r.sub = (bool (*)(void*, uint32_t))dlsym(h, "SteamAPI_ISteamApps_BIsSubscribedApp");
        r.dlc = (bool (*)(void*, uint32_t))dlsym(h, "SteamAPI_ISteamApps_BIsDlcInstalled");
        r.logged = (bool (*)(void*))dlsym(h, "SteamAPI_ISteamUser_BLoggedOn");
        auto stats = acces("SteamAPI_SteamUserStats_v", 13);
        r.store = (bool (*)(void*))dlsym(h, "SteamAPI_ISteamUserStats_StoreStats");
        if (!init || !user || !apps || !sid || !r.sub || !r.dlc || !r.logged || !stats || !r.store) {
            fprintf(stderr, "[STEAM-REEL] missing in %s:%s%s%s%s%s%s%s%s%s\n", lib.c_str(), init ? "" : " InitFlat",
                    user ? "" : " SteamUser_v023+", apps ? "" : " SteamApps_v009+", sid ? "" : " GetSteamID",
                    r.sub ? "" : " BIsSubscribedApp", r.dlc ? "" : " BIsDlcInstalled", r.logged ? "" : " BLoggedOn",
                    stats ? "" : " SteamUserStats_v013+", r.store ? "" : " StoreStats");
            steam_reel_refus(steam_texte(kSteamAncien));
        }
        setenv("SteamAppId", std::to_string(kSteamAppId).c_str(), 1);
        char err[1024] = {0};
        if (init(err) != 0)
            steam_reel_refus(steam_texte(kSteamInit) + " (Steam: " + err + ")");
        r.apps = apps();
        r.user = user();
        r.stats = stats();
        r.id = sid(r.user);
        if (!r.apps || !r.id || !r.sub(r.apps, kSteamAppId))
            steam_reel_refus(steam_texte(kSteamPasPossede));
        fprintf(stderr, "[STEAM-REEL] compte Steam connecté, jeu %u possédé\n", kSteamAppId);
        return r;
    }();
    return r;
}
static void impl_SteamAPI_Init(State& s) { steam_reel(); s.gpr.rax.dword = 1; win32_cdecl_ret(s); }
BINK_STUB(SteamAPI_RegisterCallback)
BINK_STUB(SteamAPI_Shutdown)
BINK_STUB(SteamAPI_UnregisterCallback)
// t6zm asks Steam for an encrypted app ticket before it authenticates to Demonware (0x53c2a8 -> 0x5d8360:
// ISteamUser::RequestEncryptedAppTicket, slot 20, then SteamAPI_RegisterCallResult on the handle), polls
// SteamAPI_RunCallbacks until its CCallResult (vtable 0xbd6528, slot 0 = Run(param, ioFailure, call) -> 0x49e280)
// has stored EncryptedAppTicketResponse_t::m_eResult, then reads the ticket (slot 21). The three calls were no-ops:
// the result was never delivered. Ticket: the caller's data as is (no Demonware responder in this copy reads it).
extern "C" bool win32_run_lifted(State* s, uint32_t va);
extern "C" uint32_t win32_guest_alloc(uint32_t size, uint32_t align);
static std::mutex g_steam_mu;                                    // the five below
static uint8_t g_steam_app_ticket[128];
static uint32_t g_steam_ticket_len;
static uint64_t g_steam_calls;                                   // last SteamAPICall_t handed out
static std::vector<uint64_t> g_steam_done;                       // calls whose result waits for RunCallbacks
static std::unordered_map<uint64_t, uint32_t> g_steam_results;   // call -> the guest CCallbackBase registered for it
static uint32_t g_steam_ticket_response;                               // guest EncryptedAppTicketResponse_t { k_EResultOK }
static void impl_SteamAPI_RegisterCallResult(State& s) {             // (pCallback, hAPICall), cdecl
    std::lock_guard<std::mutex> lock(g_steam_mu);
    g_steam_results[win32_arg(s, 1) | (uint64_t)win32_arg(s, 2) << 32] = win32_arg(s, 0);
    win32_cdecl_ret(s);
}
static void impl_SteamAPI_UnregisterCallResult(State& s) {
    std::lock_guard<std::mutex> lock(g_steam_mu);
    g_steam_results.erase(win32_arg(s, 1) | (uint64_t)win32_arg(s, 2) << 32);
    win32_cdecl_ret(s);
}
static void impl_SteamAPI_RunCallbacks(State& s) {
    std::vector<std::pair<uint64_t, uint32_t>> run;                  // delivered outside the lock: guest code runs
    {
        std::lock_guard<std::mutex> lock(g_steam_mu);
        for (auto it = g_steam_done.begin(); it != g_steam_done.end();) {
            const auto handler = g_steam_results.find(*it);
            if (handler == g_steam_results.end()) { ++it; continue; }
            run.emplace_back(*it, handler->second);
            g_steam_results.erase(handler);
            it = g_steam_done.erase(it);
        }
        if (!run.empty() && !g_steam_ticket_response) {
            g_steam_ticket_response = win32_guest_alloc(8, 4);
            win32_mem_write32(g_steam_ticket_response, 1);
        }
    }
    for (const auto& [call, callback] : run) {                       // callback->Run(&response, false, call), thiscall
        const uint32_t save_esp = s.gpr.rsp.dword;
        uint32_t sp = save_esp;
        sp -= 4; win32_mem_write32(sp, (uint32_t)(call >> 32));
        sp -= 4; win32_mem_write32(sp, (uint32_t)call);
        sp -= 4; win32_mem_write32(sp, 0);
        sp -= 4; win32_mem_write32(sp, g_steam_ticket_response);
        sp -= 4; win32_mem_write32(sp, 0xDEAD0000u);                 // return sentinel
        s.gpr.rsp.dword = sp;
        s.gpr.rcx.dword = callback;
        const bool ok = win32_run_lifted(&s, win32_mem_read32(win32_mem_read32(callback)));
        s.gpr.rsp.dword = save_esp;
        fprintf(stderr, "[STEAM] resultat de l'appel %llu livre a 0x%08x (Run 0x%08x)%s\n", (unsigned long long)call,
                callback, win32_mem_read32(win32_mem_read32(callback)), ok ? "" : " ECHEC");
    }
    win32_cdecl_ret(s);
}
// Keeps the id the game hands to SteamAPI_RestartAppIfNecessary; ISteamUtils::GetAppID (slot 9) returns it. As a stub the id read back was 0 (Z-NATIF-14u).
static std::atomic<uint32_t> g_steam_appid;
static void impl_SteamAPI_RestartAppIfNecessary(State& s) {
    g_steam_appid = win32_arg(s, 0);
    fprintf(stderr, "[STEAM] RestartAppIfNecessary(%u) -> 0\n", win32_arg(s, 0));
    s.gpr.rax.dword = 0;
    win32_cdecl_ret(s);
}
// ISteamFriends::GetPersonaName (slot 0) returns a 0x40-byte buffer filled at start. Unanswered,
// the game copied its gamertag from guest address 0 (0x409058) and showed the default of its name dvars,
// "Unknown Soldier". BO2_NAME replaces it.
extern "C" const char* win32_game_dir();
static uint32_t steam_persona_name() {
    static const uint32_t buf = [] {
        char name[0x40] = "Player";
        const char* env = getenv("BO2_NAME");
        FILE* f = nullptr;
        if (env && *env) snprintf(name, sizeof name, "%s", env);
        char line[512], key[64], val[256];
        bool steam = false;
        // ponytail: `key = "value"` or `key = value` on one line; an unquoted value keeps its trailing blanks and
        // an empty one is not recognised (the default stays). The file of the install only has the quoted form.
        while (f && fgets(line, sizeof line, f)) {
            if (line[0] == '[') { steam = !strncasecmp(line, "[Steam]", 7); continue; }
            if (!steam || (sscanf(line, " %63[^= \t] = \"%255[^\"\r\n]", key, val) != 2 &&
                           sscanf(line, " %63[^= \t] = %255[^\r\n]", key, val) != 2) || strcasecmp(key, "Username")) continue;
            snprintf(name, sizeof name, "%s", val);
            break;
        }
        if (f) fclose(f);
        const char* source = env && *env ? "BO2_NAME" : "defaut";
        // The player's Steam name, from Steam's own list of accounts: PersonaName of the block marked MostRecent, else
        // the last one: the logged-in session is what Windows Steam would answer.
        if (!(env && *env)) {
            const char* home = getenv("HOME");
            FILE* v = home ? fopen((std::string(home) + "/Library/Application Support/Steam/config/loginusers.vdf").c_str(), "r") : nullptr;
            char persona[256] = "", recent[8];
            while (v && fgets(line, sizeof line, v)) {
                if (sscanf(line, " \"PersonaName\" \"%255[^\"]", val) == 1) snprintf(persona, sizeof persona, "%s", val);
                else if (sscanf(line, " \"MostRecent\" \"%7[^\"]", recent) == 1 && recent[0] == '1') break;
            }
            if (v) fclose(v);
            if (persona[0]) { snprintf(name, sizeof name, "%s", persona); source = "loginusers.vdf"; }
            else if (getenv("USER")) { snprintf(name, sizeof name, "%s", getenv("USER")); source = "USER"; }
        }
        const uint32_t g = win32_guest_alloc(sizeof name, 4);
        for (uint32_t i = 0; i < sizeof name; i++) win32_mem_write8(g + i, (uint8_t)name[i]);
        fprintf(stderr, "[STEAM] GetPersonaName -> \"%s\" (%s)\n", name, source);
        return g;
    }();
    return buf;
}

// --- Steam interface emulation (SteamUser/SteamUtils/... getters) ---
// The flat-API getters (SteamUser(), SteamUtils(), ...) must return non-NULL
// C++ interface objects, or the title's Steam gate (FUN_0058a830) pops
// WIN_NO_STEAM. Each getter returns a guest object {vtable_ptr}; the vtable is
// STEAM_VTBL_SLOTS sentinels of the form STEAM_VTBL_BASE|(iface<<8)|slot.
// __remill_function_call routes an indirect `call [vtable+slot*4]` to
// steam_vtbl_dispatch() below, which decodes (iface,slot) and answers only the
// methods the game's checks depend on (measure -> implement); all others log
// loudly and return 0. thiscall = callee cleans args, so each handler pops the
// right count. See disasm of FUN_0058a830 / CSteamID validator 0x5c0140.
extern "C" uint32_t win32_guest_alloc(uint32_t size, uint32_t align);
extern "C" void win32_guest_free(uint32_t addr);
extern "C" int gfx_texture_dims(uint32_t obj, uint32_t* w, uint32_t* h);
extern "C" void gfx_release_texture(uint32_t obj);
extern "C" const char* gfx_cbmap_for(uint32_t obj);
extern "C" const char* gfx_texmap_for(uint32_t obj);
extern "C" int gfx_texslot_at(uint32_t obj, int i, uint32_t* mtl, uint32_t* d3d);
extern "C" uint32_t gfx_cbreg_for_mtl(uint32_t obj, uint32_t mtl);
extern "C" int bo2_dxbc_cbuffer_layout(const void* dxbc, uint32_t len,
                                       const char* cbname, char** out);
extern "C" void     win32_mem_read_block(void* dst, uint32_t va, uint32_t n);
extern "C" uint8_t win32_mem_read8(uint32_t a);

// Frames presented (IDXGISwapChain::Present). The no-progress runaway guard in
// win32_runtime.cpp reads this: a menu presenting frames is alive, not spinning.
extern "C" uint32_t g_present_frames = 0;
extern "C" int win32_current_tid();

// --- 2D-UI GEOMETRY CAPTURE (BO2_GEO_DUMP=1) --------------------------------
// The D3D11 layer is stubbed (no GPU, no pixels), but the guest still uploads
// real vertex/index buffers and issues real DrawIndexed. With a GPU we'd render;
// short of that, capture the geometry at Draw time and reconstruct the menu
// layout offline (tools reads geo_dump.txt). GATED so default runs are unchanged:
// only when on do buffers get PER-OBJECT backing (else Map aliases one scratch,
// so VB/IB overwrite each other and no coherent geometry survives to Draw).
extern "C" uint8_t  win32_mem_read8(uint32_t a);
extern "C" uint32_t win32_mem_read32(uint32_t a);
extern "C" void     win32_mem_write32(uint32_t a, uint32_t v);
extern "C" uint32_t win32_guest_alloc(uint32_t size, uint32_t align);
static int g_geo = [] { const char* e = bo2_getenv("BO2_GEO_DUMP"); return e ? atoi(e) : 0; }();
// Live Metal window (Gate B): feed real uploaded textures to the presenter.
static int g_window_gfx = [] { return bo2_env_on("BO2_WINDOW") ? 1 : 0; }();
// State tracking (buffer backing, bound texture, viewport) is needed by BOTH the
// offline capture and the live Metal path, so gate it on either. Only the FILE
// dump stays behind g_geo alone.
// Declared AFTER both flags, so plain static init is enough (no ordering trap).
static const int g_track = (g_geo || g_window_gfx) ? 1 : 0;
struct GeoBuf { uint32_t backing, size, bind; };
static std::unordered_map<uint32_t, GeoBuf> g_geo_bufs;   // resource obj ptr -> backing
static uint32_t g_geo_vb = 0, g_geo_vb_stride = 0, g_geo_vb_off = 0;
static uint32_t g_geo_ib = 0, g_geo_ib_fmt = 0, g_geo_ib_off = 0;
// Live path IA state (tracked whenever g_track, independent of the dump).
static uint32_t g_ui_vb = 0, g_ui_stride = 0, g_ui_vboff = 0;
// PER-SLOT vertex streams. D3D11 binds up to 32; the room's material draws use
// two (positions plus a packed attribute), and collapsing them onto one record
// is what made 46% of the scene's draws read positions at a 4-byte stride.
enum { kMaxVBSlots = 8 };
static unsigned long long g_mipc_bc = 0, g_mipc_plain = 0;
static unsigned long long g_mipc_bcfmt[100] = {0};
static uint32_t g_vb_slot[kMaxVBSlots]   = {0};
static uint32_t g_vb_stride[kMaxVBSlots] = {0};
static uint32_t g_vb_off[kMaxVBSlots]    = {0};
static std::vector<uint8_t> g_scratch_vb2[3];   // staging for streams 1..3
static uint32_t g_ui_ib = 0, g_ui_ibfmt = 0, g_ui_iboff = 0;
// Dynamic-buffer Map ring: obj -> the address its most recent Map returned. Reset
// each Present. Bounded, unlike a per-object allocation.
// Created-shader identity, so the number of shaders the menu actually BINDS can
// be measured (the created count is a superset: the engine builds the whole
// frontend set up front).
static std::unordered_map<uint32_t, char> g_sh_obj;
static uint32_t g_sh_pending_len = 0;
static std::vector<uint8_t> g_sh_bc_pending;              // bytecode of the shader being created
static std::unordered_map<uint32_t, std::vector<uint8_t>> g_sh_bc;   // obj -> DXBC
static std::unordered_map<uint32_t, int> g_sh_done;       // obj -> already translated
#include <array>
#include <set>

extern "C" void bo2_hostyield_report();
extern "C" void bo2_filscpu_report();
static std::set<uint32_t> g_font_ps;                     // pixel shaders that sample the glyph atlas
static std::set<uint32_t> g_lutgen_ps;                   // the colour-grading LUT generator
// ...AND ITS CONSUMER. The generator is fully exonerated (real 1024x128 input,
// live constants, correct cbuffer map, real 1024x32 output target), so the fault
// is in whoever SAMPLES that 1024x32 LUT. The tone-map composite is identifiable
// from its own arithmetic, not from a name: an unrolled 32-cube LUT in a
// 1024-wide texture is addressed with 1/1024 = 0.0009765625, and its v is
// 31/32 * b + 0.5/32 = 0.96875 / 0.015625. Both constants together are the
// signature. Names are recorded per Metal texture index so the log can say which
// SAMPLER a resource lands on — the whole question here is a renumbering one.
static std::set<uint32_t> g_lutuse_ps;
static std::set<uint32_t> g_fog_vs;                      // vertex shaders that compute fog
// 3D textures the guest created, so UpdateSubresource can size its upload. The
// baked-lighting volume the whole menu room is lit by is created with NO initial
// data and filled later, so without this it stays a stand-in forever.
struct Tex3DDesc { uint32_t w, h, d, fmt; };
static std::unordered_map<uint32_t, Tex3DDesc> g_tex3d_desc;
static std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> g_cb_layout;
static std::unordered_map<uint32_t, std::vector<std::string>> g_tex_names;  // ps -> mtl idx -> name
// The resource the generator last wrote into, resolved from its RTV. What the
// composite must be reading, and the one value that turns this from an argument
// into a measurement.
static uint32_t g_lut_rt_res = 0;
// Constant buffer 0 of the vertex stage. The translated vertex shaders show what
// the engine actually does with it:
//   struct PerObjectConsts { float4x4 worldViewMatrix;            // +0
//                            float4x4 worldViewProjectionMatrix;  // +64  ... }
//   clip.x = dot(float4(POSITION0,1), worldViewProjectionMatrix[0]); ...
// So the transform we were missing is 64 bytes at offset 64 of VS CB0. Reading it
// per draw and applying it ourselves gives correct screen positions without
// having to build a full pipeline per shader pair first.
extern "C" char* bo2_msl_attributes(const char* msl);
extern "C" int   bo2_dxbc_translate(const void* dxbc, uint32_t len, char** out_msl,
                                    char** out_texmap, char** out_cbmap);
extern "C" int   bo2_dxbc_translate2(const void* dxbc, uint32_t len, char** out_msl,
                                     char** out_texmap, char** out_cbmap,
                                     char** out_texdim, char** out_smpmap);
extern "C" void  gfx_register_smpslots(uint32_t obj, const char* map);
extern "C" void  gfx_register_sampler(uint32_t obj, const uint32_t desc[13]);
extern "C" void  gfx_register_texdims(uint32_t obj, const char* map);
extern "C" void  gfx_register_texslots(uint32_t obj, const char* map);
extern "C" void  gfx_register_texture_mips(uint32_t obj, uint32_t w, uint32_t h,
                                           uint32_t fmt, uint32_t mips, const void* data,
                                           uint32_t bytes, const uint32_t* offsets,
                                           const uint32_t* pitches);
extern "C" void  gfx_register_cube_mips(uint32_t obj, uint32_t size, uint32_t fmt,
                                        uint32_t mips, const void* data, uint32_t bytes,
                                        const uint32_t* offsets, const uint32_t* pitches);
extern "C" void  gfx_register_texture_vol(uint32_t obj, uint32_t w, uint32_t h,
                                          uint32_t depth, uint32_t faces, uint32_t fmt,
                                          const void* data, uint32_t bytes,
                                          uint32_t pitch, uint32_t slice_pitch);
extern "C" void  gfx_register_cbslots(uint32_t obj, const char* map);
// One fully-described draw, so Metal can run BO2's OWN shaders instead of our
// fixed-function stand-in. Everything here is what the guest actually bound.
struct GfxDrawElem { char semantic[24]; uint32_t fmt, offset, slot; };
// Must match kMaxPSTex in runtime/gfx/metal_backend.mm. Four slots was a guess;
// a D3D11 pixel stage has 128 and BO2 binds well past t3.
enum { kMaxPSTex = 16 };
// Constant-buffer slots per stage. BO2's UI shaders read b0..b2; D3D11 allows 14.
enum { kMaxCB = 8 };
// One D3D11_RENDER_TARGET_BLEND_DESC, flattened. The write MASK is the field that
// matters most: BO2 runs passes whose pixel shader writes a constant, with colour
// writes masked off, so leaving the mask at "write everything" turns a masked
// depth/stencil pass into an opaque fill. One such shader (a fragment that returns
// float4(1,1,1,1) and takes no inputs) was painting the whole window white.
struct GfxBlend {
    uint32_t enable;
    uint32_t src_rgb, dst_rgb, op_rgb;
    uint32_t src_a,   dst_a,   op_a;
    uint32_t write_mask;
};
// THE GUEST'S DEPTH-STENCIL STATE. The backend had NONE — no depth attachment, no
// depth-stencil state, no depthAttachmentPixelFormat on any pipeline — so draws
// landed in submission order and far geometry painted over near geometry. That is
// one bug with five faces: the map looked empty, the linear-Z target resolved
// black, BO2's fog factor (which is computed from depth) came out 0, every fog
// shader then output pure fog colour, and the composite graded that flat scene to
// the beige haze the campaign renders as.
struct GfxDepth {
    uint32_t enable;      // D3D11_DEPTH_STENCIL_DESC.DepthEnable
    uint32_t write;       // DepthWriteMask: 0 = ZERO, 1 = ALL
    uint32_t func;        // D3D11_COMPARISON_FUNC, 1..8
    uint32_t st;          // 0 = StencilEnable FALSE; else 1 | StencilReadMask << 8 | StencilWriteMask << 16
    uint32_t st_ops;      // 8 nibbles, D3D11 values 1..8: front FailOp, DepthFailOp, PassOp, Func; then back
    uint32_t st_ref;      // StencilRef of the last OMSetDepthStencilState (8 bits)
    uint32_t st_res;      // resource behind the bound DSV when its format has a stencil plane, else 0
};
struct GfxDraw {
    uint32_t vs, ps;                    // guest shader object ids
    const GfxDrawElem* elems; uint32_t nelems;
    const void* vb; uint32_t vb_bytes, vb_stride, vb_off;
    // EXTRA VERTEX STREAMS (slots 1..3). The input layout's elements carry the
    // slot they come from; until now every element was read out of slot 0's
    // buffer, which is only correct for single-stream draws.
    const void* vb2[3]; uint32_t vb2_bytes[3], vb2_stride[3];
    const void* ib; uint32_t ib_bytes, ib_fmt;   // fmt: 57 = R16_UINT else R32
    uint32_t idx_count, start_idx; int32_t base_vtx;
    uint32_t topo;                      // 4 = trianglelist, 5 = strip
    uint32_t rtv;                       // guest RTV object this draw targets
    uint32_t is_clear;                  // 1 = ClearRenderTargetView, not a draw
    uint32_t is_copy, copy_dst, copy_src;   // 1 = CopyResource(dst, src)
    float    clear_rgba[4];
    float    vp[6];                     // viewport x, y, w, h, MinDepth, MaxDepth
    // 🎯 MinDepth/MaxDepth ETAIENT JETES. D3D11_VIEWPORT fait six flottants
    // (24 octets) et le shim n'en lisait que quatre, puis le backend Metal
    // recollait 0..1 en dur. C'est avec ces deux champs que COD dessine le
    // modele de vue : la passe de l'arme compresse la plage de profondeur
    // pour que le fusil ne rentre jamais dans le decor. Sans eux l'arme est
    // rendue a la profondeur du monde, donc ENTERREE dans le terrain, et ne
    // reparait qu'aux endroits ou la geometrie manque — exactement ce
    // que le testeur a vu a l'ecran le 2026-09-16.
    const void* vscb[kMaxCB]; uint32_t vscb_bytes[kMaxCB];
    const void* pscb[kMaxCB]; uint32_t pscb_bytes[kMaxCB];
    uint32_t pstex[kMaxPSTex];          // guest texture object per PS slot
    GfxBlend blend;                     // the guest's own OMSetBlendState
    GfxDepth depth;                     // the guest's own OMSetDepthStencilState
    uint32_t dsv;                       // the depth-stencil view bound with the RTV
    float vpw, vph;
    // C47 : numero de remplissage du scratch de chaque CB (0 = inconnu). Meme numero que le dessin
    // precedent = memes octets : le consommateur reprend sa tranche d'arene au lieu de recopier.
    uint32_t vscb_fill[kMaxCB], pscb_fill[kMaxCB];
    // C50 : pour chaque flux (0 = slot 0, 1..3 = slots supplementaires), le debut et la taille du tampon
    // STATIQUE entier dont vb / vb2 est une tranche (nullptr = flux recopie comme avant). Le consommateur
    // ne recopie pas ces octets : ils ne changent plus (cree avec pInitialData, jamais Map).
    const void* res[4]; uint32_t res_bytes[4];
    uint32_t pssamp[kMaxPSTex];         // P805 : guest sampler state per PS s register (PSSetSamplers)
    // Rasterizer state of this draw: bit 0 = ScissorEnable (then `sc` is RSSetScissorRects[0]: left, top,
    // right, bottom), bits 8-9 = D3D11_CULL_MODE to apply (2 FRONT, 3 BACK; 0 = none).
    uint32_t rs; int32_t sc[4];
    // BO2_DEPTH_DSV: the resource behind the DSV bound at this draw (any depth format) and its size (w << 16 | h);
    // 0 = no DSV, unknown view, or lever off. is_clear == 3: the depth plane to clear (value in clear_rgba[0]).
    uint32_t dsv_res, dsv_wh;
    // BO2_DEPTH_BIAS: D3D11_RASTERIZER_DESC DepthBias, SlopeScaledDepthBias, DepthBiasClamp of this draw's state
    // (zeros with the lever off); DepthClipEnable = 0 is bit 2 of `rs`.
    int32_t zb; float zb_pente, zb_max;
    // Mip range of the SRV bound per PS slot: MostDetailedMip << 8 | MipLevels (MipLevels -1 -> 0 = all);
    // 0 = the whole chain. The downsample chains read one level of the texture they write into.
    uint32_t psmip[kMaxPSTex];
};
extern "C" void gfx_real_draw(const GfxDraw* d);
// Declared at FILE scope: a block-scope extern inside an extern "C"
// function inherits C linkage and silently fails to link.
extern "C" void gfx_real_frame_end();
extern "C" double gfx_refresh_hz();                     // P773 : rafraîchissement de l'écran (metal_backend.mm)
extern "C" void bo2_attendre_jusqua_ns(uint64_t echeance); // P773 : attente réelle (kernel32_impl.cpp)
extern "C" void gfx_register_rendertarget(uint32_t obj, uint32_t w, uint32_t h,
                                         uint32_t fmt, uint32_t mips);
// RTV -> the guest resource it views and the mip level it renders into, so Metal can find that texture.
extern "C" void gfx_register_rtv(uint32_t rtv, uint32_t res, uint32_t mip);
extern "C" void gfx_set_backbuffer_rtv(uint32_t rtv);
extern "C" void gfx_set_backbuffer_res(uint32_t res);
static std::unordered_map<uint32_t, std::vector<GfxDrawElem>> g_il;  // layout obj -> elements
static std::unordered_map<uint32_t, uint32_t> g_rtv_res;   // RTV obj -> resource obj
static uint32_t g_cur_rtv = 0;                // OMSetRenderTargets slot 0
static uint32_t g_backbuffer_rtv = 0;         // the RTV made from the swapchain buffer
static uint32_t g_backbuffer_res = 0;         // the swapchain buffer resource itself
static uint32_t g_cur_il = 0;                 // IASetInputLayout
static uint32_t g_cur_vs = 0, g_cur_ps = 0;   // VS/PSSetShader
static bool g_cur_vs_vid = true;              // C46 : g_cur_vs lit SV_VertexID (vrai tant qu'on ne sait pas)
static uint32_t g_vscb[kMaxCB] = {0}, g_pscb[kMaxCB] = {0};
static uint32_t g_pstex[kMaxPSTex] = {0};
static uint32_t g_psmip[kMaxPSTex] = {0};     // GfxDraw::psmip of the SRV bound per slot
static std::unordered_map<uint32_t, uint32_t> g_srv_mip;   // SRV obj -> GfxDraw::psmip (only when not 0)
static uint32_t g_pssamp[kMaxPSTex] = {0};    // P805 : PSSetSamplers, by s register
// Blend states the guest created, and the one currently bound. The default (no
// state bound) is D3D's: blending off, write everything.
static const GfxBlend kBlendDefault = { 0, 2, 1, 1, 2, 1, 1, 0xF };
static std::unordered_map<uint32_t, GfxBlend> g_blend_states;
static GfxBlend g_cur_blend = kBlendDefault;
// Same shape for depth. D3D's defaults when no state object is bound are
// DepthEnable=TRUE, DepthWriteMask=ALL, DepthFunc=LESS — NOT "no depth", which is
// what we were effectively doing by having no depth buffer at all.
static const GfxDepth kDepthDefault = { 1, 1, 2 };
static std::unordered_map<uint32_t, GfxDepth> g_depth_states;
static GfxDepth g_cur_depth = kDepthDefault;
static uint32_t g_cur_dsv = 0;                // OMSetRenderTargets' pDepthStencilView
// THE DEPTH CLEAR VALUE, taken from the guest instead of assumed. BO2's captured
// depth states are DepthFunc=7 (GREATER_EQUAL) almost throughout, which means a
// REVERSED-Z buffer: near is 1.0 and far is 0.0, so the correct clear is 0.0 and
// the conventional 1.0 rejects essentially every fragment. Rather than encode
// that conclusion, read the value the engine actually clears with.
static float g_depth_clear = 0.0f;
extern "C" float gfx_depth_clear_value() { return g_depth_clear; }
// La taille que le PRODUCTEUR voit, pour que le backend Metal puisse verifier
// que sa propre declaration de GfxDraw n'a pas diverge. Voir le commentaire
// sur struct GfxDraw dans runtime/gfx/metal_backend.mm.
extern "C" uint32_t gfx_draw_record_size(void) { return (uint32_t)sizeof(GfxDraw); }
// Last PSSetShaderResources seen, so a draw with no texture can be attributed to the
// bind rather than to the lookup.
static uint32_t g_last_srv_start = 0, g_last_srv_num = 0, g_last_srv_obj = 0;
static float g_vp_now[6] = {0.f, 0.f, 1920.f, 1080.f, 0.f, 1.f};
static uint32_t g_ui_vscb0 = 0;
static uint32_t g_ui_topo = 4;     // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
static uint32_t g_ui_ring_off = 0;
static std::unordered_map<uint32_t, uint32_t> g_ui_ring_map;
// Bumped every time the guest Maps a buffer, so a cache of its CONTENTS knows it
// went stale. Keying a constant-buffer cache on the address alone is wrong: the Map
// ring hands the same buffer the same slice for a whole frame while the guest
// rewrites it between draws, so every draw after the first got the first draw's
// constants — which for a per-object transform means the whole frame collapses onto
// one object's matrix.
static std::unordered_map<uint32_t, uint32_t> g_buf_gen;
static uint32_t buf_data_addr(uint32_t obj) {
    auto b = g_geo_bufs.find(obj);
    if (b != g_geo_bufs.end() && b->second.backing) return b->second.backing;
    auto r = g_ui_ring_map.find(obj);
    return (r != g_ui_ring_map.end()) ? r->second : 0;
}
static bool dyn_sans_tranche(uint32_t obj) {   // P772
    auto b = g_geo_bufs.find(obj);
    return obj && b != g_geo_bufs.end() && !b->second.backing && b->second.size && !g_ui_ring_map.count(obj);
}
static FILE*    g_geo_f = nullptr;
// Texture capture: turns the reconstruction from wireframe into a real image.
// Immutable textures arrive at CreateTexture2D with pInitialData (the fastfile
// pixels) — dump them THEN (the source may be freed after). SRV->texture and the
// bound PS texture are tracked so each Draw knows which image it samples.
struct GeoTex { uint32_t w, h, fmt, pitch; int dumped; };
static std::unordered_map<uint32_t, GeoTex> g_geo_tex;   // texture obj  -> desc
// Toute texture 2D creee, sans aucune condition d'armement : la sonde [CSR]
// en a besoin pour dire QUELLE texture recoit une copie et de quelle taille.
// g_geo_tex ne convient pas — il n'est rempli que sous BO2_GEO.
struct Tex2DShape { uint16_t w, h; uint16_t fmt, mips; };
static std::unordered_map<uint32_t, Tex2DShape> g_tex2d_wh;
static std::unordered_map<uint32_t, uint32_t> g_geo_srv; // SRV obj      -> texture obj
// D3D11 STENCIL AND RASTERIZER STATE (Z-NATIF-20). Until now CreateDepthStencilState kept the three depth
// fields, CreateRasterizerState nothing, and RSSetScissorRects was only read under BO2_GEO_DUMP: the stencil
// LUI clips its `setUseStencil` elements with, and the scissor rectangle every one of the game's rasterizer
// states enables (t6sp 0x6f8340 writes ScissorEnable = 1), never reached Metal.
static inline bool puiss2(uint64_t n) { return (n & (n - 1)) == 0; }   // witnesses print at 1, 2, 4, 8…
static bool levier_gfx(const char* var, const char* quoi, bool defaut) {
    const bool on = bo2_env_on(var, defaut);
    fprintf(stderr, "%s : %s (%s)\n", quoi, on ? "ARME" : "ETEINT", var);
    return on;
}
static const bool g_pochoir = levier_gfx("BO2_STENCIL", "[POCHOIR] pochoir D3D11 execute", true);
static const bool g_ciseau  = levier_gfx("BO2_SCISSOR", "[CISEAU] rectangle de ciseau D3D11 execute", true);
static const bool g_cull    = levier_gfx("BO2_CULL", "[CULL] CullMode D3D11 traduit", true);   // 10-21 : sans lui, le dos du
                                                                                        // couvercle du Millimeter Scanner bouchait sa vitre
// The stencil plane lives in the RESOURCE, not in the view: two DSVs of one texture share it.
static uint32_t dsv_pochoir_res(uint32_t dsv) {
    auto gs = g_geo_srv.find(dsv);
    if (gs == g_geo_srv.end()) return 0;
    auto wh = g_tex2d_wh.find(gs->second);
    if (wh == g_tex2d_wh.end()) return 0;
    const uint32_t f = wh->second.fmt;           // R32G8X24_TYPELESS / D32_FLOAT_S8X24 / R24G8_TYPELESS / D24_S8
    return (f == 19 || f == 20 || f == 44 || f == 45) ? gs->second : 0;
}
static uint32_t g_cur_dsv_res = 0;               // dsv_pochoir_res(g_cur_dsv)
// BO2_DEPTH_DSV: 1 = every pass off the backbuffer attaches the depth of the DSV the game bound, 2 = only the passes
// without a colour target (the backend reads the same variable), anything else = off.
static const int g_depth_dsv = [] {
    const char* e = bo2_getenv("BO2_DEPTH_DSV");
    const int v = e ? atoi(e) : 0;
    fprintf(stderr, "[DEPTH-DSV] profondeur attachee d'apres le DSV lie : %s (BO2_DEPTH_DSV)\n",
            v == 1 ? "ARME (toutes les passes)" : v == 2 ? "ARME (passes sans couleur seulement)" : "ETEINT");
    return (v == 1 || v == 2) ? v : 0;
}();
// The depth plane, like the stencil plane, lives in the RESOURCE. No format filter: a DSV only exists on a depth texture.
static uint32_t dsv_prof(uint32_t dsv, uint32_t* wh) {
    *wh = 0;
    if (!g_depth_dsv || !dsv) return 0;
    auto gs = g_geo_srv.find(dsv);
    auto t = gs == g_geo_srv.end() ? g_tex2d_wh.end() : g_tex2d_wh.find(gs->second);
    if (t == g_tex2d_wh.end()) {
        static uint64_t n = 0;
        if (puiss2(++n)) fprintf(stderr, "[DEPTH-DSV] DSV %08x sans texture connue : %llu\n", dsv, (unsigned long long)n);
        return 0;
    }
    *wh = (uint32_t)t->second.w << 16 | t->second.h;
    return gs->second;
}
static uint32_t g_cur_dsv_prof = 0, g_cur_dsv_wh = 0;   // dsv_prof(g_cur_dsv)
// BO2_DEPTH_BIAS=1: the rasterizer state's DepthBias / SlopeScaledDepthBias / DepthBiasClamp and DepthClipEnable = 0
// follow each draw to Metal (setDepthBias, depth clamp). They went nowhere: the game's shadow casters and decals
// were rasterised at their unbiased depth.
static const bool g_depth_bias = [] {
    const bool on = bo2_env_on("BO2_DEPTH_BIAS", false);
    fprintf(stderr, "[DEPTH-BIAS] biais de profondeur et DepthClipEnable du jeu : %s (BO2_DEPTH_BIAS)\n", on ? "ARME" : "ETEINT");
    return on;
}();
// D3D11_RASTERIZER_DESC: CullMode +4, ScissorEnable +28. NULL state = D3D11 defaults: CULL_BACK, no scissor.
struct EtatRaster { uint8_t cull, ciseau, pince_z; int32_t biais; float pente, biais_max; };   // pince_z = DepthClipEnable 0
static const EtatRaster kRasterDefaut = { 3, 0 };
static std::unordered_map<uint32_t, EtatRaster> g_raster_states;
static EtatRaster g_cur_raster = kRasterDefaut;
static int32_t g_sc_now[4] = {0, 0, 0, 0};       // RSSetScissorRects[0]; D3D11's rectangle is EMPTY until set
static bool g_sc_pose = false;
static uint64_t g_st_emis = 0, g_st_rejetes = 0;   // stencil draws handed to Metal / dropped by real_draw's early exits
static uint64_t g_st_non_exec = 0;               // stencil draws issued through a draw slot we do not execute
static uint32_t g_geo_ps_tex = 0;                        // currently-bound PS texture obj
// IEEE half -> float. The frontend's UI vertices pack their texture coordinates
// as TWO HALVES in one dword, which is why every "find a pair of floats in 0..1"
// heuristic failed on them.
static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t man  = h & 0x3ff;
    uint32_t bits;
    if (exp == 0) {
        if (!man) bits = sign;                       // +/-0
        else {                                       // subnormal -> normalise
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            man &= 0x3ff;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1f) {
        bits = sign | 0x7f800000u | (man << 13);     // inf / nan
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f; memcpy(&f, &bits, 4); return f;
}
static float geo_f32(uint32_t va) { uint32_t u = win32_mem_read32(va); float f; memcpy(&f, &u, 4); return f; }

// --- LIVE 2D UI -> Metal (Gate B step 3) ------------------------------------
// Same decode as the offline capture, but the triangles go straight to the Metal
// window instead of a text file: DrawIndexed appends to a per-frame vertex list
// tagged with the bound texture, and Present hands the finished frame over.
// Only draws whose vertices look like VIEWPORT-PIXEL 2D are taken — the 3D scene
// pass submits model-space geometry that needs the view/projection matrices we do
// not reconstruct yet, and letting that through would smear noise over the UI.
// DXBC -> Metal Shading Language, in-process. This is the piece the port could
// not do itself: BO2 ships 2518 distinct DXBC shaders (314 bound by the menu
// alone), and an element's appearance IS its pixel shader, so no amount of
// fixed-function reconstruction can produce the real menu. HLSLcc (vendored in
// third_party/, Unity's production cross-compiler) translates all 2185 dumped
// shaders with zero failures, and unlike DXVK/DXMT it is a plain C++ library —
// no Wine prefix, no Vulkan, no MoltenVK.
extern "C" int bo2_dxbc_to_msl(const void* dxbc, uint32_t len, char** out_msl);
extern "C" void bo2_msl_free(char* p);
extern "C" void gfx_register_shader2(uint32_t obj, int is_pixel, const char* msl,
                                    const char* attrs);
extern "C" bool gfx_shaders_open(void);

extern "C" void gfx_register_texture(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt,
                                     const void* data, uint32_t bytes, uint32_t pitch);
extern "C" void gfx_ui_frame(const void* verts, uint32_t nverts,
                             const void* batches, uint32_t nbatches,
                             float vpw, float vph);

// --- DYNAMIC TEXTURES: Map/Unmap delivery -----------------------------------
// A texture the guest fills through Map/Unmap instead of at creation. BO2's
// decoded video arrives this way (the WebM/libvpx path writes Y, Cr and Cb
// planes per frame), and so does anything else the engine refreshes at runtime.
// Each one gets its OWN guest-side backing so two mapped textures cannot alias,
// and Unmap is what actually pushes the bytes to Metal.
namespace {
// `depth` > 1 makes this a VOLUME. The menu room's baked-lighting texture is a
// DYNAMIC 512x256x4 3D texture (usage=2, bind=SHADER_RESOURCE, cpu=WRITE), so it
// is filled through exactly this Map/Unmap path — it is not a render target and
// not immutable, which is why it arrived at CreateTexture3D with `data=no`.
struct DynTex { uint32_t w, h, fmt, pitch, bytes, backing; uint64_t used;
                uint32_t depth = 1, slice = 0; };
std::unordered_map<uint32_t, DynTex> g_dyntex;
// References D3D11 modelisees des textures dynamiques ET des vues faites d'elles
// (voir dref_release). Seuls ces objets y entrent.
std::unordered_map<uint32_t, uint32_t> g_d3d_refs;
std::unordered_map<uint32_t, uint32_t> g_dref_vue;   // vue suivie -> sa texture
uint64_t g_dyntex_tick = 0, g_dyntex_live = 0, g_dyntex_evicted = 0;
uint64_t g_dyntex_black = 0, g_dyntex_up = 0;

// Bytes per pixel for the uncompressed formats a Map can target. Block-compressed
// formats are never CPU-written a row at a time, so they are deliberately absent:
// returning 0 makes the caller skip rather than compute a bogus pitch.
uint32_t dxgi_bpp(uint32_t fmt) {
    switch (fmt) {
        case 28: case 29: case 87: case 88: case 24: case 26: return 4;  // RGBA8/BGRA8/R10G10B10A2
        case 10: case 11: case 34: case 35:                   return 4;  // RG16
        case 2:  case 3:  case 4:                             return 16; // RGBA32
        case 54: case 55: case 56: case 57:                   return 2;  // R16
        case 61: case 65:                                     return 1;  // R8 / A8  <- video planes
        case 49: case 50: case 51:                            return 2;  // R8G8
        default:                                              return 0;
    }
}

// Every texture the shim hands the guest, logged once at creation with its
// dimensions and format. There is exactly ONE texture in a whole run that gets
// BOUND and never receives pixels — 0x180001c0, ~2 binds a frame, 6400 over a
// run — while every other one resolves. Matching that handle to a creation
// tells us what the black area of the menu actually is. BO2_TEXLOG=1.
void texlog(const char* what, uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt,
            uint32_t usage, uint32_t bind, uint32_t cpu, int has_init) {
    static const int on = bo2_getenv("BO2_TEXLOG") ? 1 : 0;
    if (!on) return;
    fprintf(stderr, "[TEX] %-8s obj=0x%08x %ux%u fmt=%u usage=%u bind=0x%x cpu=0x%x "
                    "init=%d\n", what, obj, w, h, fmt, usage, bind, cpu, has_init);
}

void dyntex_register(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t bpp = dxgi_bpp(fmt);
    if (!bpp) return;                       // not a format we can hand back a row pitch for
    uint64_t bytes = (uint64_t)w * bpp * h;
    if (!bytes || bytes > (64u << 20)) return;
    DynTex t{ w, h, fmt, w * bpp, (uint32_t)bytes, 0, 0 };
    g_dyntex[obj] = t;                      // backing allocated lazily on first Map
    g_d3d_refs[obj] = 1;                    // la reference que Create rend au jeu
}

// The same for a VOLUME. Map/Unmap is the only path a dynamic 3D texture is ever
// filled by, and the room's baked lighting is one — so without this the lighting
// volume stays the 1x1 stand-in for the whole run and the room has no diffuse
// term at all.
void dyntex_register_vol(uint32_t obj, uint32_t w, uint32_t h, uint32_t depth,
                         uint32_t fmt) {
    uint32_t bpp = dxgi_bpp(fmt);
    if (!bpp || depth < 2) return;
    uint64_t slice = (uint64_t)w * bpp * h;
    uint64_t bytes = slice * depth;
    if (!bytes || bytes > (64u << 20)) return;
    DynTex t{ w, h, fmt, w * bpp, (uint32_t)bytes, 0, 0 };
    t.depth = depth; t.slice = (uint32_t)slice;
    g_dyntex[obj] = t;
    g_d3d_refs[obj] = 1;
}

// 🎯 LES REFERENCES D3D11 D'UNE TEXTURE DYNAMIQUE, MODELISEES — pour la copie
// Metal seulement. Mesure (dr1_afghanistan, [DYNREF]) : chaque image video suit
//   Create, CreateSRV, Release | GetResource, Map, Unmap, Release |
//   PSSetShaderResources x2-3 | GetResource, GetDesc, Release | vue Release.
// Le jeu rend TOUT. Sous Windows la texture meurt avec sa derniere vue, et une
// vue vit tant que le contexte la tient liee ; ici Release ne faisait rien, et la
// copie Metal de chaque image restait (5 000+ images, ~9 Go : gfx_release_texture).
// Les references que le RUNTIME prend sont celles qui ont fait echouer les deux
// essais precedents (note d'AddRef/Release) : une vue tient sa texture,
// GetResource et QueryInterface rendent une reference, une liaison PS tient sa vue.
// ⚠️ Le TAMPON invite n'est PAS touche : il reste a l'eviction par age (Map). Ces
// essais le recyclaient au zero et cassaient le chargement ; ici un zero trop tot
// ne coute qu'une texture absente a l'ecran, et `tex_tard` ([PRESENT]) le compte.
uint32_t g_dref_ps[128];                    // vue liee a chaque slot PS (reference du contexte)

// 🎯 P1049 : le stockage invite d'un tampon mort revient a l'arene, sinon chaque carte y laisse ses tampons (~80 Mo)
// et la campagne enchainee finit en « Out of memory » a Haiti. Delai : c50_lier cle ses copies Metal par adresse et
// taille, et ne les evince qu'apres 4 096 a 8 192 images ; rendre plus tot pourrait relier une copie perimee.
// Un tampon encore lie reste vivant : le contexte D3D11 tient une reference sur ses liaisons.
// ponytail: delai en images, pas une invalidation explicite ; l'arene garde 2 a 3 niveaux de marge.
constexpr uint32_t kTamponMortImages = 8300;
struct TamponMort { uint32_t obj, image; };
std::vector<TamponMort> g_tampons_morts;

static void tampons_morts_rendre() {
    auto lie = [](uint32_t o) {
        if (o == g_ui_vb || o == g_ui_ib) return true;
        for (uint32_t v : g_vb_slot) if (v == o) return true;
        for (uint32_t v : g_vscb) if (v == o) return true;
        for (uint32_t v : g_pscb) if (v == o) return true;
        return false;
    };
    auto mort = [&](const TamponMort& t) {
        if (g_present_frames - t.image < kTamponMortImages || lie(t.obj)) return false;
        auto b = g_geo_bufs.find(t.obj);
        if (b != g_geo_bufs.end()) {
            win32_guest_free(b->second.backing);
            b->second.backing = 0;
        }
        return true;
    };
    g_tampons_morts.erase(std::remove_if(g_tampons_morts.begin(), g_tampons_morts.end(), mort), g_tampons_morts.end());
}

void dref_add(uint32_t o) {
    auto it = g_d3d_refs.find(o);
    if (it != g_d3d_refs.end()) it->second++;
}

void dref_release(uint32_t o) {
    auto it = g_d3d_refs.find(o);
    if (it == g_d3d_refs.end() || --it->second) return;
    g_d3d_refs.erase(it);
    auto v = g_dref_vue.find(o);
    if (v != g_dref_vue.end()) {     // une vue meurt : elle lache sa texture
        uint32_t tex = v->second;
        g_dref_vue.erase(v);
        dref_release(tex);
        return;
    }
    auto tampon = g_geo_bufs.find(o);
    if (tampon != g_geo_bufs.end()) {       // un tampon meurt (P1049)
        if (tampon->second.backing) g_tampons_morts.push_back({o, g_present_frames});
        return;
    }
    gfx_release_texture(o);                 // la texture meurt
}

void dref_view(uint32_t vue, uint32_t res) {
    auto it = g_d3d_refs.find(res);
    if (it == g_d3d_refs.end()) return;
    it->second++;                           // la vue tient sa ressource
    g_d3d_refs[vue] = 1;
    g_dref_vue[vue] = res;
}

// [DYNREF] — CE QUE LE JEU FAIT D'UNE TEXTURE VIDEO ENTRE DEUX IMAGES.
// Chaque image decodee arrive dans une texture dynamique NEUVE (plus de 5 000
// dans un run afghanistan, 1920x720 A8), et le cote Metal n'en rend aucune :
// ~9 des 12,8 Go. Windows ne detruit une ressource qu'a refcount zero, et D3D
// prend des references a lui (vue -> ressource, liaison -> vue) : avant d'en
// modeliser une seule, savoir QUELS appels atteignent une texture dynamique et
// les vues faites d'elle, dans quel ordre, depuis quel site invite. Les 400
// premiers evenements un par un avec l'adresse de retour invitee, puis des
// totaux toutes les 250 creations. Aucun effet sur le jeu. BO2_DYNREF=1.
const bool g_dynref_on = bo2_getenv("BO2_DYNREF") != nullptr;
std::unordered_map<uint32_t, uint32_t> g_dynref_view;      // vue -> texture dynamique
std::unordered_map<uint32_t, uint64_t> g_dynref_desc_site; // site invite -> GetDesc ressource
uint64_t g_dynref_n[16][128];                               // [iface][slot], `this` suivi
uint64_t g_dynref_ev = 0, g_dynref_crees = 0, g_dynref_vues = 0;
uint64_t g_dynref_map = 0, g_dynref_unmap = 0, g_dynref_lie = 0;

void dynref_event(const char* quoi, uint32_t self, uint32_t tex, int iface, int slot,
                  uint32_t site) {
    if (++g_dynref_ev > 400) return;
    auto t = g_dyntex.find(tex);
    fprintf(stderr, "[DYNREF-EV] #%llu %s obj=%08x tex=%08x %ux%u fmt=%u if=%d slot=%d "
                    "ret=%08x\n", (unsigned long long)g_dynref_ev, quoi, self, tex,
            t != g_dyntex.end() ? t->second.w : 0, t != g_dyntex.end() ? t->second.h : 0,
            t != g_dyntex.end() ? t->second.fmt : 0, iface, slot, site);
}

void dynref_summary() {
    auto n = [](int i, int s) { return (unsigned long long)g_dynref_n[i][s]; };
    const int R = 7, V = 8;   // D3D_IF_RESOURCE, D3D_IF_VIEW (enum plus bas)
    fprintf(stderr, "[DYNREF] crees=%llu vues=%llu | tex qi=%llu addref=%llu release=%llu "
                    "gettype=%llu getdesc=%llu | vue qi=%llu addref=%llu release=%llu "
                    "getres=%llu | ctx map=%llu unmap=%llu lie=%llu | getdesc_sites=%zu",
            (unsigned long long)g_dynref_crees, (unsigned long long)g_dynref_vues,
            n(R, 0), n(R, 1), n(R, 2), n(R, 7), n(R, 10), n(V, 0), n(V, 1), n(V, 2), n(V, 7),
            (unsigned long long)g_dynref_map, (unsigned long long)g_dynref_unmap,
            (unsigned long long)g_dynref_lie, g_dynref_desc_site.size());
    int k = 0;
    for (auto& kv : g_dynref_desc_site)
        if (k++ < 6) fprintf(stderr, " %08x:%llu", kv.first, (unsigned long long)kv.second);
    fprintf(stderr, "\n");
}

void dynref_created(uint32_t obj, uint32_t site) {
    if (!g_dynref_on || !g_dyntex.count(obj)) return;
    dynref_event("cree", obj, obj, 0, 5, site);
    if ((++g_dynref_crees % 250) == 0) dynref_summary();
}

void dynref_view(uint32_t view, uint32_t res, int slot, uint32_t site) {
    if (!g_dynref_on || !g_dyntex.count(res)) return;
    g_dynref_view[view] = res; g_dynref_vues++;
    dynref_event("vue-creee", view, res, 4, slot, site);
}

// En tete de d3d_vtbl_dispatch : tout appel dont `this` est suivi, plus Map /
// Unmap / PSSetShaderResources du contexte sur un objet suivi.
void dynref_note(int iface, int slot, uint32_t self, uint32_t a1, uint32_t a2, uint32_t a3,
                 uint32_t site) {
    if (iface == 7 && slot == 10) g_dynref_desc_site[site]++;   // GetDesc, TOUTE ressource
    uint32_t tex = 0; const char* quoi = "tex";
    if (g_dyntex.count(self)) tex = self;
    else { auto v = g_dynref_view.find(self);
           if (v != g_dynref_view.end()) { tex = v->second; quoi = "vue"; } }
    if (tex) {
        if (iface >= 0 && iface < 16 && slot >= 0 && slot < 128) g_dynref_n[iface][slot]++;
        dynref_event(quoi, self, tex, iface, slot, site);
        return;
    }
    if (iface == 5 && (slot == 14 || slot == 15) && g_dyntex.count(a1)) {
        (slot == 14 ? g_dynref_map : g_dynref_unmap)++;
        dynref_event(slot == 14 ? "map" : "unmap", a1, a1, iface, slot, site);
        return;
    }
    if (iface == 5 && slot == 8 && a3) {                     // PSSetShaderResources
        for (uint32_t i = 0; i < a2 && i < 128; i++) {
            auto v = g_dynref_view.find(win32_mem_read32(a3 + i * 4));
            if (v == g_dynref_view.end()) continue;
            g_dynref_lie++;
            dynref_event("lie", v->first, v->second, iface, slot, site);
        }
    }
}
}  // namespace
struct UiVert  { float x, y, z, u, v; uint8_t r, g, b, a; };
struct UiBatch {
    uint32_t first, count, tex;
    uint32_t has_mtx;      // 0 = positions are already in viewport pixels
    float    mtx[16];      // row-major: clip.x = dot(pos, mtx[0..3]), etc.
};
static std::vector<UiVert>  g_ui_v;
static std::vector<UiBatch> g_ui_b;
static float  g_ui_vpw = 1920.f, g_ui_vph = 1080.f;
static int    g_ui_probe = [] { const char* e = bo2_getenv("BO2_UI_PROBE"); return e ? atoi(e) : 0; }();

// Dump a texture's raw bytes to geo_tex_<obj>.bin (once) and a metadata line.
static void geo_dump_tex(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt,
                         uint32_t dataVA, uint32_t pitch) {
    auto it = g_geo_tex.find(obj);
    if (it != g_geo_tex.end() && it->second.dumped) return;
    g_geo_tex[obj] = { w, h, fmt, pitch, 0 };
    if (!dataVA || !w || !h || w > 8192 || h > 8192) return;
    // byte size: uncompressed = pitch*h; if pitch unknown, guess by format below
    // in the Python decoder. Dump min(pitch*h, a sane cap).
    uint32_t bytes = pitch ? pitch * h : w * h * 4;
    if (bytes > (32u << 20)) return;                     // skip huge (>32MB)
    char path[64]; snprintf(path, sizeof path, "geo_tex_%08x.bin", obj);
    FILE* tf = fopen(path, "wb"); if (!tf) return;
    // Bulk read + one fwrite. The per-byte read8+fputc version spent >20 s on the
    // 25 MB atlas and tripped the stall watchdog, which read as an engine spin.
    { std::vector<uint8_t> buf(bytes);
      win32_mem_read_block(buf.data(), dataVA, bytes);
      fwrite(buf.data(), 1, bytes, tf); }
    fclose(tf);
    g_geo_tex[obj].dumped = 1;
    if (g_geo_f) { fprintf(g_geo_f, "X obj=%08x w=%u h=%u fmt=%u pitch=%u bytes=%u\n",
                           obj, w, h, fmt, pitch, bytes); fflush(g_geo_f); }
}
// Write a distinct state line (scissor/viewport/clear) once — these repeat every
// frame, so dedup collapses them to the actual set of UI regions. Capped.
static void geo_state(const char* fmt, ...) {
    if (!g_geo_f) return;
    char buf[128]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    static std::unordered_map<std::string,int> seen; static int n = 0;
    if (n >= 4000 || seen.count(buf)) return;
    seen[buf] = 1; n++;
    fputs(buf, g_geo_f); fflush(g_geo_f);
}
// Dump the triangles of one DrawIndexed, decoding the first two floats of each
// vertex as (x,y) and the dword at stride-4 as a candidate RGBA colour. Only a
// window of frames is captured so the file stays small.
static void geo_dump_draw(uint32_t frame, uint32_t drawn, uint32_t idxCount,
                          uint32_t startIdx, uint32_t baseVtx) {
    if (!g_geo_vb || !g_geo_ib || !g_geo_vb_stride) return;
    uint32_t vbd = buf_data_addr(g_geo_vb), ibd = buf_data_addr(g_geo_ib);
    if (!vbd || !ibd) return;
    static std::unordered_map<uint64_t,int> seen;   // dedup identical triangles
    static int total = 0;
    if (total >= 20000) return;                     // global cap
    if (!g_geo_f) { g_geo_f = fopen("geo_dump.txt", "w"); if (!g_geo_f) { g_geo = 0; return; } }
    uint32_t i16 = (g_geo_ib_fmt == 57u /*R16_UINT*/ || g_geo_ib_fmt == 0) ? 2u : 4u;
    uint32_t ibbase = ibd + g_geo_ib_off + startIdx * i16;
    uint32_t vbbase = vbd + g_geo_vb_off;
    uint32_t st = g_geo_vb_stride;
    uint32_t tris = idxCount / 3; if (tris > 4000) tris = 4000;   // cap per draw
    for (uint32_t t = 0; t < tris; t++) {
        uint32_t vi[3]; float x[3], y[3], u[3], vv[3]; uint32_t col = 0;
        for (int k = 0; k < 3; k++) {
            uint32_t ia = ibbase + (t * 3 + k) * i16;
            vi[k] = (i16 == 2) ? (win32_mem_read8(ia) | (win32_mem_read8(ia + 1) << 8))
                               : win32_mem_read32(ia);
            uint32_t v = vbbase + (baseVtx + vi[k]) * st;
            x[k] = geo_f32(v); y[k] = geo_f32(v + 4);
            // UVs: the common 2D-UI vertex is pos.xy, then a colour dword, then uv;
            // capture the two floats right after pos as candidate UVs (offset 8/12
            // or 12/16 depending on layout) — the decoder picks whichever is in 0..1.
            u[k]  = st >= 16 ? geo_f32(v + 8)  : 0.f;
            vv[k] = st >= 16 ? geo_f32(v + 12) : 0.f;
            if (st >= 8 && k == 0) col = win32_mem_read32(v + (st >= 12 ? st - 4 : 4));
        }
        // dedup: hash the three (x,y) pairs
        uint64_t h = 1469598103934665603ull;
        for (int k = 0; k < 3; k++) { uint32_t ux,uy; memcpy(&ux,&x[k],4); memcpy(&uy,&y[k],4);
            h = (h ^ ux) * 1099511628211ull; h = (h ^ uy) * 1099511628211ull; }
        if (seen.count(h)) continue;
        // skip fully-degenerate (all three points identical) triangles
        if (x[0]==x[1] && x[1]==x[2] && y[0]==y[1] && y[1]==y[2]) continue;
        seen[h] = 1; total++;
        fprintf(g_geo_f, "T %.3f %.3f %.3f %.3f %.3f %.3f %08x tex=%08x uv %.4f %.4f %.4f %.4f %.4f %.4f f%u\n",
                x[0], y[0], x[1], y[1], x[2], y[2], col, g_geo_ps_tex,
                u[0], vv[0], u[1], vv[1], u[2], vv[2], frame);
    }
    fflush(g_geo_f);
}

// Decode one DrawIndexed into this frame's UI vertex list.
// Vertex layout is not uniform across the frontend's draws, so decide per draw
// from the data rather than assuming one: position is always the first two
// floats, and the two floats after it are taken as UVs ONLY when they are in
// range for all three corners of every triangle. When they are not (the radial
// meshes carry something else there), the batch is drawn untextured against the
// 1x1 white texture, which still gives the correct shape and colour.
static void ui_collect_draw(uint32_t idxCount, uint32_t startIdx, uint32_t baseVtx) {
    if (!g_window_gfx || !g_ui_vb || !g_ui_ib || !g_ui_stride) return;
    uint32_t vbd = buf_data_addr(g_ui_vb), ibd = buf_data_addr(g_ui_ib);
    if (!vbd || !ibd) return;
    if (g_ui_v.size() > 200000u) return;                  // per-frame sanity cap
    uint32_t i16 = (g_ui_ibfmt == 57u /*R16_UINT*/ || g_ui_ibfmt == 0) ? 2u : 4u;
    uint32_t ibbase = ibd + g_ui_iboff + startIdx * i16;
    uint32_t vbbase = vbd + g_ui_vboff;
    uint32_t st = g_ui_stride;
    // Topology matters: reading a STRIP as a LIST produces exactly the giant
    // fan-shaped triangles the first live frame showed. 4 = TRIANGLELIST,
    // 5 = TRIANGLESTRIP; anything else (points/lines) is not ours to draw.
    uint32_t tris;
    if (g_ui_topo == 4)      tris = idxCount / 3;
    else if (g_ui_topo == 5) tris = idxCount >= 3 ? idxCount - 2 : 0;
    else return;
    if (!tris || tris > 20000) return;

    // UV offset is layout-dependent. Two are in use in the frontend (probed):
    //   stride 32: float2 pos; float2 uv;  uint32 col     -> uv at +8
    //   stride 36: float4 pos; uint32 col; ...            -> uv is NOT at +8
    // Rather than hard-code, try each candidate and keep the first whose values
    // are in texture range for every corner of this draw.
    // UV forms actually used by the frontend (probed on real draws):
    //   stride 32: float2 uv at +8            (the quad/scene-image path)
    //   stride 36: HALF2 uv at +20, u = high half, v = low half
    // Derived from a glyph quad: pos (131,286)->(147,286)->(147,302) with +20 =
    // 343834e3 / 37c734e3 / 37c73838, i.e. the high half tracks x and the low
    // half tracks y, both landing in 0..1 (0.264->0.486, 0.305->0.527).
    static const uint32_t kUvCand[] = { 8, 12, 16, 24 };

    // First pass: read the triangles, and check whether the UV candidates and
    // the positions are usable.
    struct V { float x, y, z, u, v; uint32_t col; };
    std::vector<V> tmp;
    tmp.reserve(tris * 3);
    // Choose the UV offset once per draw, from the first triangle's corners.
    // Try the half2 form first: it is what the text/icon draws use.
    uint32_t uvoff = 0;
    bool uv_half = false;
    if (st >= 24) {
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            uint32_t ia = ibbase + (uint32_t)k * i16;
            uint32_t vi = (i16 == 2) ? (uint32_t)(win32_mem_read8(ia) |
                                                  (win32_mem_read8(ia + 1) << 8))
                                     : win32_mem_read32(ia);
            uint32_t d  = win32_mem_read32(vbbase + (baseVtx + vi) * st + 20);
            float u = half_to_float((uint16_t)(d >> 16));
            float v = half_to_float((uint16_t)(d & 0xffff));
            if (!(u > -0.02f && u < 1.02f && v > -0.02f && v < 1.02f)) ok = false;
        }
        if (ok) { uvoff = 20; uv_half = true; }
    }
    for (uint32_t c = 0; !uvoff && c < sizeof(kUvCand) / sizeof(kUvCand[0]); c++) {
        uint32_t off = kUvCand[c];
        if (off + 8 > st) continue;
        bool ok = true;
        for (int k = 0; k < 3 && ok; k++) {
            uint32_t ia = ibbase + k * i16;
            uint32_t vi = (i16 == 2) ? (uint32_t)(win32_mem_read8(ia) |
                                                  (win32_mem_read8(ia + 1) << 8))
                                     : win32_mem_read32(ia);
            uint32_t va = vbbase + (baseVtx + vi) * st;
            float u = geo_f32(va + off), v = geo_f32(va + off + 4);
            if (!(u > -0.02f && u < 1.02f && v > -0.02f && v < 1.02f)) ok = false;
        }
        if (ok) { uvoff = off; break; }
    }
    bool uv_ok = uvoff != 0;
    // What separates 2D UI from the 3D passes, from the captured geometry:
    // the UI is small AXIS-ALIGNED quads at sane pixel positions (the frontend's
    // glyph/icon draws are exact 16x16 rectangles like (131,286)-(147,302)),
    // while the 3D command-center geometry is a handful of triangles spanning
    // most of the screen. Filtering on the vertex LAYOUT was wrong — the real UI
    // is the stride-36 form (float4 pos; uint32 col; ...), and keying on stride 32
    // threw the menu away and kept the fans. So filter on SHAPE instead, per
    // triangle. BO2_UI_LOOSE=1 disables it for comparison.
    static const int loose = bo2_getenv("BO2_UI_LOOSE") ? 1 : 0;
    const float big_w = g_ui_vpw * 0.6f, big_h = g_ui_vph * 0.6f;
    for (uint32_t t = 0; t < tris; t++) {
        V tri[3];
        bool tri_ok = true;
        for (int k = 0; k < 3; k++) {
            // A strip's triangle t is (t, t+1, t+2) with alternating winding; a
            // list's is (3t, 3t+1, 3t+2).
            uint32_t ii = (g_ui_topo == 5) ? (t + (uint32_t)k) : (t * 3 + (uint32_t)k);
            uint32_t ia = ibbase + ii * i16;
            uint32_t vi = (i16 == 2) ? (uint32_t)(win32_mem_read8(ia) |
                                                  (win32_mem_read8(ia + 1) << 8))
                                     : win32_mem_read32(ia);
            uint32_t va = vbbase + (baseVtx + vi) * st;
            V o;
            o.x = geo_f32(va); o.y = geo_f32(va + 4);
            o.z = st >= 12 ? geo_f32(va + 8) : 0.f;
            if (!uv_ok)          { o.u = o.v = 0.f; }
            else if (uv_half)    { uint32_t d = win32_mem_read32(va + uvoff);
                                   o.u = half_to_float((uint16_t)(d >> 16));
                                   o.v = half_to_float((uint16_t)(d & 0xffff)); }
            else                 { o.u = geo_f32(va + uvoff);
                                   o.v = geo_f32(va + uvoff + 4); }
            o.col = st >= 20 ? win32_mem_read32(va + 16) : 0xffffffffu;
            // A 2D UI vertex is inside the viewport (in pixels). Filter PER
            // TRIANGLE, not per draw: rejecting a whole draw because one corner
            // is off-screen threw the entire menu away (714 batches -> 2), while
            // accepting a whole draw because one corner is on-screen let the 3D
            // command-center dial (coordinates -780..+780 about the origin) smear
            // giant gradient triangles over everything. Per-triangle keeps the UI
            // and drops the scene.
            if (!(o.x > -64.f && o.x < g_ui_vpw + 64.f &&
                  o.y > -64.f && o.y < g_ui_vph + 64.f))
                tri_ok = false;
            tri[k] = o;
        }
        if (tri_ok && !loose && tris > 2) {
            // A UI element is bounded; a screen-spanning triangle inside a
            // multi-triangle draw is scene geometry, not UI. A 1-2 triangle draw
            // is allowed to be fullscreen — that IS how the background quad and
            // full-screen fades are submitted.
            float minx = tri[0].x, maxx = tri[0].x, miny = tri[0].y, maxy = tri[0].y;
            for (int k = 1; k < 3; k++) {
                if (tri[k].x < minx) minx = tri[k].x;
                if (tri[k].x > maxx) maxx = tri[k].x;
                if (tri[k].y < miny) miny = tri[k].y;
                if (tri[k].y > maxy) maxy = tri[k].y;
            }
            if (maxx - minx > big_w || maxy - miny > big_h) tri_ok = false;
        }
        if (tri_ok && !loose && tris > 2 && st >= 12) {
            // 2D UI is submitted at ONE depth: the captured glyph quads all carry
            // the same z (592.0 for that draw, 546.0 for another). The 3D passes
            // vary z per vertex. This is the discriminator that position range and
            // vertex stride both failed to be.
            if (tri[0].z != tri[1].z || tri[1].z != tri[2].z) tri_ok = false;
        }
        if (tri_ok) { tmp.push_back(tri[0]); tmp.push_back(tri[1]); tmp.push_back(tri[2]); }
    }
    if (tmp.empty()) return;

    UiBatch b{};
    b.first = (uint32_t)g_ui_v.size();
    b.count = (uint32_t)tmp.size();
    b.tex   = uv_ok ? g_geo_ps_tex : 0u;
    // worldViewProjectionMatrix out of VS CB0 (+64). A row of all zeros means the
    // engine has not filled it for this draw, in which case fall back to treating
    // the positions as viewport pixels rather than collapsing everything to a point.
    uint32_t cb = buf_data_addr(g_ui_vscb0);
    if (cb) {
        float m[16]; int nonzero = 0;
        for (int i = 0; i < 16; i++) {
            m[i] = geo_f32(cb + 64 + (uint32_t)i * 4);
            if (m[i] != 0.f) nonzero = 1;
        }
        if (nonzero) { b.has_mtx = 1; memcpy(b.mtx, m, sizeof m); }
    }
    for (const V& o : tmp) {
        UiVert w;
        w.x = o.x; w.y = o.y; w.z = o.z;
        w.u = uv_ok ? o.u : 0.f;
        w.v = uv_ok ? o.v : 0.f;
        // Layout confirmed by BO2_UI_PROBE on the frontend's draws:
        //   stride 32 = float2 pos; float2 uv; uint32 colour; ...
        // (vert0 read 0,0 / 0,1 / ffffffff). Channel ORDER is still unproven —
        // every colour seen so far is opaque white, where R8G8B8A8 and B8G8R8A8
        // are indistinguishable. Taking byte order as R,G,B,A; if the menu ever
        // comes out with red and blue swapped, this is the line.
        if (uv_ok) {
            w.r = (uint8_t)(o.col & 0xff);
            w.g = (uint8_t)((o.col >> 8) & 0xff);
            w.b = (uint8_t)((o.col >> 16) & 0xff);
            w.a = (uint8_t)((o.col >> 24) & 0xff);
            if (!w.a) w.a = 255;          // a zero-alpha vertex would be invisible
        } else {
            // Colour offset is only confirmed for the layout whose UVs we found.
            // Guessing it for the others tinted the whole menu, which reads as a
            // rendering bug rather than a missing field.
            w.r = w.g = w.b = w.a = 255;
        }
        g_ui_v.push_back(w);
    }
    g_ui_b.push_back(b);

    if (g_ui_probe) {
        static std::unordered_map<uint32_t,int> seen_stride;
        uint32_t key = st * 4 + (tris > 2 ? 1u : 0u);
        if (seen_stride[key]++ == 0) {
            fprintf(stderr, "[UI-PROBE] stride=%u idxFmt=%u topo=%u tex=%08x "
                            "uvoff=%u tris=%u\n", st, g_ui_ibfmt, g_ui_topo,
                    g_geo_ps_tex, uvoff, tris);
            // Three vertices of the first triangle, every field. One vertex is not
            // enough to spot the UV pair: it is the field that VARIES across the
            // corners of a quad while staying in 0..1.
            for (int k = 0; k < 3; k++) {
                uint32_t ia = ibbase + (uint32_t)k * i16;
                uint32_t vi = (i16 == 2) ? (uint32_t)(win32_mem_read8(ia) |
                                                      (win32_mem_read8(ia + 1) << 8))
                                         : win32_mem_read32(ia);
                uint32_t vv = vbbase + (baseVtx + vi) * st;
                fprintf(stderr, "  [v%d idx=%u]", k, vi);
                for (uint32_t o = 0; o < st && o < 64; o += 4)
                    fprintf(stderr, " +%u=%08x(%.4f)", o, win32_mem_read32(vv + o),
                            geo_f32(vv + o));
                fprintf(stderr, "\n");
            }
        }
    }
}

// Hand one DrawIndexed to Metal with everything the guest bound, so BO2's own
// translated shaders run. Buffers are copied out of guest memory here (the pump
// runs on another thread and the guest will overwrite them next Map).
static std::vector<uint8_t> g_scratch_vb, g_scratch_ib;
// ── [VB-COPIE] — combien d'octets invites `real_draw` recopie par image ──────
// Lecture seule, inconditionnel, deux additions par dessin. Voir P520.
static uint64_t g_vbcopie_octets = 0, g_vbcopie_dessins = 0, g_vbcopie_decales = 0;   // C46
// P772 : un tampon DYNAMIQUE n'a d'adresse que l'image où le jeu l'a mappé (tranche d'anneau, rendue à chaque
// Present). Sous Windows son contenu reste jusqu'au Map suivant. Un dessin qui lit un tampon dynamique non remappé
// cette image est jeté (sommets/index) ou part avec un tampon de constantes non lié (indéfini sous Metal). Cumuls,
// imprimés sur [VB-COPIE] : perdus_dessins= (VB/IB, dessin jeté), perdus_flux= (flux 1-3), perdus_cb= (CB liés).
static uint64_t g_dynperdu_dessins = 0, g_dynperdu_flux = 0, g_dynperdu_cb = 0;
static uint64_t g_vbcopie_residents = 0, g_c50_statiques_mappes = 0;                  // C50
static std::vector<uint8_t> g_scratch_cb[2 * kMaxCB];
// C47 : chaque relecture d'un scratch de CB recoit un numero neuf (jamais 0) ; voir GfxDraw::vscb_fill.
static uint32_t g_cb_fill = 0, g_cb_fill_of[2 * kMaxCB] = {0};
static uint32_t cb_fill_neuf() { if (++g_cb_fill == 0) ++g_cb_fill; return g_cb_fill; }
// 🎯 C35 (2026-09-26) — LES SOMMETS SE LISENT EN PLACE. La memoire invitee est UNE
// carte plate (win32_runtime.cpp, guest_mem_init) : bo2_g_base + va EST l'octet
// invite, et gfx_real_draw le recopie aussitot dans l'arene de l'image. Passer par
// g_scratch_vb faisait une copie de plus par dessin (sample p675 222758, fil backend :
// win32_mem_read_block 6,6 %, pour ~36 Mo par image). Au-dela des 4 Gio, la copie
// reste et replie l'adresse comme avant. Les INDEX, eux, passent encore par
// g_scratch_ib : le max qui dimensionne la fenetre de sommets doit etre lu dans les
// octets memes qui partent au GPU, pas dans une memoire qu'un autre fil peut reecrire.
extern "C" uint8_t* bo2_g_base;
static const uint8_t* guest_view(uint32_t va, uint32_t n, std::vector<uint8_t>& scratch) {
    if ((uint64_t)va + n <= (1ull << 32)) return bo2_g_base + va;
    scratch.resize(n);
    win32_mem_read_block(scratch.data(), va, n);
    return scratch.data();
}
// C50 : un tampon est STATIQUE s'il a ete cree avec pInitialData (il a son propre stockage) et n'a jamais
// ete Map (g_buf_gen, +1 a chaque Map). Ses octets sont alors ceux de sa creation, pour toujours.
static bool tampon_statique(uint32_t obj, uint32_t* va, uint32_t* n) {
    const auto b = g_geo_bufs.find(obj);
    if (b == g_geo_bufs.end() || !b->second.backing || !b->second.size) return false;
    const auto g = g_buf_gen.find(obj);
    if (g != g_buf_gen.end() && g->second) return false;
    if ((uint64_t)b->second.backing + b->second.size > (1ull << 32)) return false;
    *va = b->second.backing; *n = b->second.size;
    return true;
}
// Le plus petit et le plus grand index de la fenetre, huit (ou quatre) index par instruction. Ce
// fichier est compile en -O1, sans vectoriseur : la boucle octet par octet d'avant
// faisait l'essentiel des 6,4 % propres de real_draw (sample p675). `ib` vient de
// g_scratch_ib (un std::vector, donc aligne).
// 🎯 C46 (P683, 2026-09-27) — LA FENETRE DE SOMMETS PART DE L'INDEX MINIMAL. Elle partait du sommet 0 du
// tampon : un dessin qui lit les sommets 50 000-51 000 d'un grand tampon partage en recopiait 51 000, deux fois
// (gfx_real_draw dans l'arene sur le fil backend, puis upload() dans le MTLBuffer sur le fil AppKit). Capture
// xctrace p682 055238 : memmove = 22 % du backend et 22 % d'AppKit, arene 60 Mo par image lourde dont 36,5 Mo
// de sommets et d'index ([VB-COPIE], [C35]). On copie [baseVtx+mini, baseVtx+maxi], on retranche mini des
// index copies et le dessin part avec baseVertex 0 : le GPU lit W[i-mini] = VB[baseVtx+i], le MEME octet invite
// qu'avant, sur tous les flux (tous sont lus par sommet, instanceCount vaut 1). Seul [[vertex_id]] changerait :
// un VS qui declare SV_VertexID garde l'ancienne fenetre, comme les strips (index de redemarrage 0xFFFF) et un
// baseVtx negatif. BO2_VB_FENETRE=0 rend l'ancien chemin. Temoin : `decales=` dans [VB-COPIE].
static uint32_t ib_min_max(const uint8_t* ib, uint32_t count, uint32_t isz, uint32_t* mini) {
    uint32_t k = 0;
    if (isz == 2) {
        const uint16_t* q = reinterpret_cast<const uint16_t*>(ib);
        uint16x8_t hi = vdupq_n_u16(0), lo = vdupq_n_u16(0xffff);
        for (; k + 8 <= count; k += 8) {
            const uint16x8_t v = vld1q_u16(q + k);
            hi = vmaxq_u16(hi, v); lo = vminq_u16(lo, v);
        }
        uint16_t m = vmaxvq_u16(hi), n = vminvq_u16(lo);
        for (; k < count; k++) { m = q[k] > m ? q[k] : m; n = q[k] < n ? q[k] : n; }
        *mini = n;
        return m;
    }
    const uint32_t* q = reinterpret_cast<const uint32_t*>(ib);
    uint32x4_t hi = vdupq_n_u32(0), lo = vdupq_n_u32(0xffffffffu);
    for (; k + 4 <= count; k += 4) {
        const uint32x4_t v = vld1q_u32(q + k);
        hi = vmaxq_u32(hi, v); lo = vminq_u32(lo, v);
    }
    uint32_t m = vmaxvq_u32(hi), n = vminvq_u32(lo);
    for (; k < count; k++) { m = q[k] > m ? q[k] : m; n = q[k] < n ? q[k] : n; }
    *mini = n;
    return m;
}
static void ib_rebase(uint8_t* ib, uint32_t count, uint32_t isz, uint32_t mini) {
    uint32_t k = 0;
    if (isz == 2) {
        uint16_t* q = reinterpret_cast<uint16_t*>(ib);
        const uint16x8_t d = vdupq_n_u16((uint16_t)mini);
        for (; k + 8 <= count; k += 8) vst1q_u16(q + k, vsubq_u16(vld1q_u16(q + k), d));
        for (; k < count; k++) q[k] = (uint16_t)(q[k] - mini);
        return;
    }
    uint32_t* q = reinterpret_cast<uint32_t*>(ib);
    const uint32x4_t d = vdupq_n_u32(mini);
    for (; k + 4 <= count; k += 4) vst1q_u32(q + k, vsubq_u32(vld1q_u32(q + k), d));
    for (; k < count; k++) q[k] -= mini;
}
// Le nom du semantique est dans la signature d'entree (ISGN) du DXBC ; la casse y est celle du HLSL.
// Bytecode inconnu : on repond « lit », donc l'ancienne fenetre.
static bool vs_reads_vertex_id(uint32_t so) {
    static std::unordered_map<uint32_t, bool> memo;
    const auto m = memo.find(so);
    if (m != memo.end()) return m->second;
    bool lit = true;
    const auto it = g_sh_bc.find(so);
    if (it != g_sh_bc.end() && !it->second.empty()) {
        static const char kNom[] = "sv_vertexid";
        const size_t n = sizeof kNom - 1;
        const std::vector<uint8_t>& bc = it->second;
        lit = false;
        for (size_t i = 0; !lit && i + n <= bc.size(); i++) {
            size_t k = 0;
            while (k < n && (char)tolower(bc[i + k]) == kNom[k]) k++;
            lit = (k == n);
        }
    }
    memo[so] = lit;
    return lit;
}
// [RD-SORTIE] : un dessin du jeu jete en silence avant d'atteindre Metal (P1013).
static void rd_sortie(const char* why, uint32_t idxCount) {
    static uint64_t n = 0;
    if (puiss2(++n)) fprintf(stderr, "[RD-SORTIE] %s cumul=%llu idx=%u vs=%08x ps=%08x il=%08x vb=%08x ib=%08x stride=%u rtv=%08x\n", why, (unsigned long long)n,
                             idxCount, g_cur_vs, g_cur_ps, g_cur_il, g_ui_vb, g_ui_ib, g_ui_stride, g_cur_rtv);
}
static void real_draw(uint32_t idxCount, uint32_t startIdx, int32_t baseVtx) {
    if (g_cur_vs && !g_cur_ps && g_cur_dsv_prof && idxCount) {   // D3D11 would still write depth
        static uint64_t n = 0;
        if (puiss2(++n)) fprintf(stderr, "[DEPTH-DSV] dessins SANS pixel shader jetes avec un DSV lie : %llu (rtv=%08x res=%08x)\n",
                                 (unsigned long long)n, g_cur_rtv, g_cur_dsv_prof);
    }
    if (!g_cur_vs || !g_cur_ps || !g_ui_stride || !idxCount) { rd_sortie(!g_cur_ps ? "sans_ps" : !g_cur_vs ? "sans_vs" : !g_ui_stride ? "sans_stride" : "idx0", idxCount); return; }
    // Copying the bound buffers out of guest memory per draw is the expensive
    // part, and the first version tripped the stall watchdog: with ~3350 draws a
    // frame, an unbounded vertex window (its size comes from the largest index,
    // which is garbage for a buffer the guest has not filled) meant tens of MB
    // per draw. Bound both the per-draw copy and the per-frame draw count, and
    // COUNT what gets dropped so this never looks like missing geometry.
    static uint32_t per_frame = 0, last_frame = ~0u;
    static uint64_t drop_cap = 0, drop_big = 0, drop_rt = 0;
    // Only what the engine aims at the swapchain buffer belongs on screen.
    // BO2_UI_ALLRT=1 submits every pass, which is what produced a window filled
    // by an offscreen pass painted over the UI.
    // BO2_UI_BBONLY=1 keeps the old "backbuffer only" behaviour; by default every
    // pass is forwarded and Metal renders it into its own target, because the menu
    // is composited from offscreen ones (16383 draws target the backbuffer, 200001
    // do not).
    static const int bb_only = bo2_getenv("BO2_UI_BBONLY") ? 1 : 0;
    if (bb_only && g_backbuffer_rtv && g_cur_rtv != g_backbuffer_rtv) {
        if ((++drop_rt % 200000) == 1)
            fprintf(stderr, "[gfx] %llu draws skipped: not targeting the backbuffer\n",
                    (unsigned long long)drop_rt);
        return;
    }
    if (last_frame != g_present_frames) {
        // [VB-COPIE] : le bilan de l'image qui vient de finir. Il parle AUSSI
        // quand le volume est petit — c'est le temoin negatif, sans lui un zero
        // et une sonde morte se lisent pareil.
        static uint64_t nrep = 0;
        if ((++nrep % 60) == 1)
            fprintf(stderr, "[VB-COPIE] image %u : %llu dessins, %.2f Mo recopies depuis "
                            "la memoire invitee (%.1f ko par dessin) decales=%llu (C46) residents=%llu "
                            "statiques_mappes=%llu (C50) perdus_dessins=%llu perdus_flux=%llu perdus_cb=%llu (P772)\n",
                    g_present_frames, (unsigned long long)g_vbcopie_dessins,
                    (double)g_vbcopie_octets / (1024.0 * 1024.0),
                    g_vbcopie_dessins ? (double)g_vbcopie_octets / 1024.0
                                        / (double)g_vbcopie_dessins : 0.0,
                    (unsigned long long)g_vbcopie_decales, (unsigned long long)g_vbcopie_residents,
                    (unsigned long long)g_c50_statiques_mappes, (unsigned long long)g_dynperdu_dessins,
                    (unsigned long long)g_dynperdu_flux, (unsigned long long)g_dynperdu_cb);
        g_vbcopie_octets = 0; g_vbcopie_dessins = 0; g_vbcopie_decales = 0; g_vbcopie_residents = 0;
        last_frame = g_present_frames; per_frame = 0;
    }
    // The per-frame cap bounds host work, but it is a HEAD cut: the frontend issues
    // ~3350 draws and the UI is drawn LAST, so a cap of 200 silently threw the whole
    // menu away — the font shader was identified and translated yet not one of its
    // draws ever reached this function. BO2_UI_MAXDRAWS overrides it (0 = no cap);
    // pair a high value with BO2_STALL_ABORT=0, since the cost is what the watchdog
    // was reacting to.
    static const uint32_t cap = [] {
        const char* e = bo2_getenv("BO2_UI_MAXDRAWS");
        // Default high enough to cover a whole frontend frame (~3350 draws). A low
        // cap is not a throttle, it is a HEAD CUT that removes the UI entirely.
        return e ? (uint32_t)strtoul(e, nullptr, 10) : 8000u;
    }();
    if (cap && per_frame >= cap) { drop_cap++; return; }
    auto il = g_il.find(g_cur_il);
    if (il == g_il.end() || il->second.empty()) { rd_sortie("sans_il", idxCount); return; }
    uint32_t vbd = buf_data_addr(g_ui_vb), ibd = buf_data_addr(g_ui_ib);
    if (!vbd || !ibd) {
        rd_sortie(!vbd ? "sans_vb" : "sans_ib", idxCount);
        if (dyn_sans_tranche(g_ui_vb) || dyn_sans_tranche(g_ui_ib)) g_dynperdu_dessins++;
        return;
    }
    uint32_t i16 = (g_ui_ibfmt == 57u || g_ui_ibfmt == 0) ? 2u : 4u;
    if (idxCount > 200000u) return;
    // Copy exactly the index range this draw uses, plus a vertex window sized
    // from the largest index in it (a whole dynamic buffer can be megabytes).
    uint32_t ib_bytes = idxCount * i16;
    g_scratch_ib.resize(ib_bytes);
    win32_mem_read_block(g_scratch_ib.data(), ibd + g_ui_iboff + startIdx * i16, ib_bytes);
    // 🎯 C50 (P685) — LES SOMMETS STATIQUES RESTENT SUR LE GPU. Quand tous les flux que lit ce dessin
    // viennent de tampons statiques (tampon_statique), on ne balaie pas les index et on ne copie pas de
    // fenetre : on passe le tampon entier, que le backend Metal cree une fois et garde. Le decalage
    // IASetVertexBuffers doit etre multiple de 4 (setVertexBuffer:offset: avec un descripteur de sommets).
    // ETEINT par defaut depuis P685 (AppKit 0,86x, copies 0,44x, mais img/s B/A 0,95-0,97) : BO2_VB_RESIDENT=1
    // l'arme. Temoin : `residents=` sur [VB-COPIE], ligne [C50] cote AppKit.
    static const bool c50 = [] {
        const char* e = bo2_getenv("BO2_VB_RESIDENT");
        const bool on = e && atoi(e) != 0;
        fprintf(stderr, "[C50] tampons de sommets statiques residents cote Metal : %s (BO2_VB_RESIDENT)\n",
                on ? "ARMES" : "ETEINTS");
        return on;
    }();
    bool want[4] = {true, false, false, false};
    for (const GfxDrawElem& el : il->second)
        if (el.slot >= 1 && el.slot <= 3) want[el.slot] = true;
    uint32_t res_va[4] = {0, 0, 0, 0}, res_n[4] = {0, 0, 0, 0};
    bool resident = c50;
    for (uint32_t sl = 0; sl < 4 && resident; sl++) {
        if (!want[sl]) continue;
        const uint32_t obj = sl ? g_vb_slot[sl] : g_ui_vb, off = sl ? g_vb_off[sl] : g_ui_vboff;
        resident = obj && (sl ? g_vb_stride[sl] : g_ui_stride) && tampon_statique(obj, &res_va[sl], &res_n[sl])
                   && off < res_n[sl] && (off & 3) == 0;
    }
    uint32_t mini = 0;
    const uint32_t maxi = resident ? 0 : ib_min_max(g_scratch_ib.data(), idxCount, i16, &mini);
    static const bool c46 = [] {
        const char* e = bo2_getenv("BO2_VB_FENETRE");
        const bool on = !e || atoi(e) != 0;
        fprintf(stderr, "[C46] fenetre de sommets depuis l'index minimal : %s (BO2_VB_FENETRE)\n",
                on ? "ARMEE" : "ETEINTE");
        return on;
    }();
    // C46 : `s` = premier sommet invite de la fenetre, `nvtx` = sa taille en sommets.
    const bool decale = !resident && c46 && mini > 0 && baseVtx >= 0 && g_ui_topo != 5 && !g_cur_vs_vid;
    const uint32_t s = decale ? (uint32_t)baseVtx + mini : 0;
    const uint32_t nvtx = decale ? maxi - mini + 1 : maxi + 1 + (baseVtx > 0 ? (uint32_t)baseVtx : 0);
    if (decale) { ib_rebase(g_scratch_ib.data(), idxCount, i16, mini); ++g_vbcopie_decales; }
    uint64_t vb_bytes = resident ? (uint64_t)(res_n[0] - g_ui_vboff) : (uint64_t)nvtx * g_ui_stride;   // C50
    if (!resident && (!vb_bytes || vb_bytes > (2u << 20))) { rd_sortie("vb_trop_gros", idxCount); drop_big++; return; }
    per_frame++;
    if ((drop_cap + drop_big) && ((drop_cap + drop_big) % 20000) == 1)
        fprintf(stderr, "[gfx] real_draw dropped: %llu over per-frame cap, "
                        "%llu with an implausible vertex window\n",
                (unsigned long long)drop_cap, (unsigned long long)drop_big);
    const uint8_t* vbp = resident ? bo2_g_base + res_va[0] + g_ui_vboff   // C50 : vbd == res_va[0]
                                  : guest_view(vbd + g_ui_vboff + s * g_ui_stride, (uint32_t)vb_bytes,
                                               g_scratch_vb);   // C35, C46
    g_vbcopie_octets += (resident ? 0 : vb_bytes) + ib_bytes;   // [VB-COPIE], voir P520
    ++g_vbcopie_dessins;
    if (resident) ++g_vbcopie_residents;

    GfxDraw d{};
    d.vs = g_cur_vs; d.ps = g_cur_ps;
    d.elems = il->second.data(); d.nelems = (uint32_t)il->second.size();
    d.vb = vbp; d.vb_bytes = (uint32_t)vb_bytes;
    d.vb_stride = g_ui_stride; d.vb_off = 0;
    // THE EXTRA STREAMS. Only the slots this input layout actually reads from
    // are copied; a slot no element references costs nothing.
    {
        for (uint32_t sl = 1; sl <= 3; sl++) {
            d.vb2[sl - 1] = nullptr; d.vb2_bytes[sl - 1] = 0;
            d.vb2_stride[sl - 1] = g_vb_stride[sl];
            if (!want[sl] || !g_vb_slot[sl] || !g_vb_stride[sl]) continue;
            if (resident) {   // C50
                d.vb2[sl - 1] = bo2_g_base + res_va[sl] + g_vb_off[sl];
                d.vb2_bytes[sl - 1] = res_n[sl] - g_vb_off[sl];
                continue;
            }
            uint32_t bd = buf_data_addr(g_vb_slot[sl]);
            if (!bd) { if (dyn_sans_tranche(g_vb_slot[sl])) g_dynperdu_flux++; continue; }
            uint64_t nb = (uint64_t)nvtx * g_vb_stride[sl];
            if (!nb || nb > (2u << 20)) continue;
            d.vb2[sl - 1] = guest_view(bd + g_vb_off[sl] + s * g_vb_stride[sl], (uint32_t)nb,
                                       g_scratch_vb2[sl - 1]);   // C35, C46
            d.vb2_bytes[sl - 1] = (uint32_t)nb;
        }
    }
    d.ib = g_scratch_ib.data(); d.ib_bytes = ib_bytes; d.ib_fmt = i16 == 2 ? 57u : 42u;
    d.idx_count = idxCount; d.start_idx = 0; d.base_vtx = decale ? 0 : baseVtx;   // C46
    for (uint32_t sl = 0; sl < 4; sl++) {   // C50
        d.res[sl] = (resident && want[sl] && res_n[sl]) ? bo2_g_base + res_va[sl] : nullptr;
        d.res_bytes[sl] = d.res[sl] ? res_n[sl] : 0;
    }
    d.topo = g_ui_topo;
    d.rtv  = g_cur_rtv;
    // FONT DRAW AUDIT: everything the shader could possibly be missing, side by side
    // with what it asks for. Its MSL declares one texture and one sampler and reads
    // TEXCOORD0 + a colour; if all of that is present and it still writes nothing,
    // the problem is not the input state.
    if (g_font_ps.count(g_cur_ps)) {
        static uint32_t nfd = 0;
        if (++nfd <= 10) {
            uint32_t vbd2 = buf_data_addr(g_ui_vb);
            fprintf(stderr, "[FONT] draw #%u idx=%u stride=%u rtv=%08x(bb=%08x) "
                            "lastSRV(start=%u num=%u obj=%08x known=%d) vp=%.0fx%.0f\n",
                    nfd, idxCount, g_ui_stride, g_cur_rtv, g_backbuffer_rtv,
                    g_last_srv_start, g_last_srv_num, g_last_srv_obj,
                    (int)(g_geo_srv.count(g_last_srv_obj) ? 1 : 0),
                    g_vp_now[2], g_vp_now[3]);
            fprintf(stderr, "[FONT]   tex:");
            for (int s = 0; s < kMaxPSTex; s++)
                if (g_pstex[s]) fprintf(stderr, " t%d=%08x", s, g_pstex[s]);
            fprintf(stderr, "\n");
            // and the first vertex, so a degenerate position is visible here
            if (vbd2) {
                uint32_t v0 = vbd2 + g_ui_vboff + (baseVtx > 0 ? (uint32_t)baseVtx : 0) * g_ui_stride;
                fprintf(stderr, "[FONT]   v0:");
                for (uint32_t o = 0; o < g_ui_stride && o < 40; o += 4)
                    fprintf(stderr, " +%u=%.3f", o, geo_f32(v0 + o));
                fprintf(stderr, "\n");
            }
        }
    }
    // The engine's final on-screen step is among these. Describe the first few so
    // the composite draw and the view it binds can be identified.
    if (g_backbuffer_rtv && g_cur_rtv == g_backbuffer_rtv) {
        static uint32_t nbb = 0;
        if (++nbb <= 24)
            fprintf(stderr, "[gfx] BACKBUFFER draw #%u idx=%u stride=%u vs=%08x ps=%08x "
                            "tex=[%08x %08x %08x %08x] vp=%.0fx%.0f\n",
                    nbb, idxCount, g_ui_stride, g_cur_vs, g_cur_ps,
                    g_pstex[0], g_pstex[1], g_pstex[2], g_pstex[3],
                    g_vp_now[2], g_vp_now[3]);
    }
    for (int i = 0; i < 6; i++) d.vp[i] = g_vp_now[i];
    d.vpw = g_ui_vpw; d.vph = g_ui_vph;
    // Constant buffers are the expensive part: BO2's are large (PerSceneConsts
    // alone is hundreds of floats) and re-reading all eight for EVERY draw starved
    // the guest thread enough for the watchdog to escalate from a stall warning to
    // aborting the run. Consecutive draws in a pass share their constant buffers,
    // so only re-read one when its bound object or address actually changed.
    static uint32_t last_vs_obj[kMaxCB] = {0}, last_ps_obj[kMaxCB] = {0};
    static uint32_t last_vs_addr[kMaxCB] = {0}, last_ps_addr[kMaxCB] = {0};
    static uint32_t last_vs_gen[kMaxCB] = {0}, last_ps_gen[kMaxCB] = {0};
    for (int i = 0; i < kMaxCB; i++) {
        uint32_t a = buf_data_addr(g_vscb[i]);
        auto b = g_geo_bufs.find(g_vscb[i]);
        uint32_t n = (b != g_geo_bufs.end()) ? b->second.size : 0;
        if (a && n && n <= (1u << 20)) {
            uint32_t gen = g_buf_gen[g_vscb[i]];
            if (g_vscb[i] != last_vs_obj[i] || a != last_vs_addr[i] ||
                gen != last_vs_gen[i] || g_scratch_cb[i].size() != n) {
                g_scratch_cb[i].resize(n);
                win32_mem_read_block(g_scratch_cb[i].data(), a, n);
                g_cb_fill_of[i] = cb_fill_neuf();   // C47
                last_vs_obj[i] = g_vscb[i]; last_vs_addr[i] = a; last_vs_gen[i] = gen;
            }
            d.vscb[i] = g_scratch_cb[i].data(); d.vscb_bytes[i] = n; d.vscb_fill[i] = g_cb_fill_of[i];
        }
        else if (!a && dyn_sans_tranche(g_vscb[i])) g_dynperdu_cb++;
        a = buf_data_addr(g_pscb[i]);
        b = g_geo_bufs.find(g_pscb[i]);
        n = (b != g_geo_bufs.end()) ? b->second.size : 0;
        if (a && n && n <= (1u << 20)) {
            uint32_t gp = g_buf_gen[g_pscb[i]];
            if (g_pscb[i] != last_ps_obj[i] || a != last_ps_addr[i] ||
                gp != last_ps_gen[i] || g_scratch_cb[kMaxCB + i].size() != n) {
                g_scratch_cb[kMaxCB + i].resize(n);
                win32_mem_read_block(g_scratch_cb[kMaxCB + i].data(), a, n);
                g_cb_fill_of[kMaxCB + i] = cb_fill_neuf();   // C47
                last_ps_obj[i] = g_pscb[i]; last_ps_addr[i] = a; last_ps_gen[i] = gp;
            }
            d.pscb[i] = g_scratch_cb[kMaxCB + i].data(); d.pscb_bytes[i] = n;
            d.pscb_fill[i] = g_cb_fill_of[kMaxCB + i];
        }
        else if (!a && dyn_sans_tranche(g_pscb[i])) g_dynperdu_cb++;
    }
    for (int i = 0; i < kMaxPSTex; i++) d.pstex[i] = g_pstex[i];
    for (int i = 0; i < kMaxPSTex; i++) d.psmip[i] = g_psmip[i];
    for (int i = 0; i < kMaxPSTex; i++) d.pssamp[i] = g_pssamp[i];
    d.blend = g_cur_blend;
    d.depth = g_cur_depth;
    d.dsv   = g_cur_dsv;
    d.dsv_res = g_cur_dsv_prof; d.dsv_wh = g_cur_dsv_wh;
    // No DSV, or a DSV whose format has no stencil plane: D3D11 neither tests nor writes stencil.
    d.depth.st_res = g_cur_dsv_res;
    if (d.depth.st && !g_cur_dsv_res) {
        d.depth.st = 0;
        static uint64_t n = 0;
        if (puiss2(++n))
            fprintf(stderr, "[POCHOIR] dessins a pochoir SANS plan de pochoir lie (dsv=%08x) : %llu\n",
                    g_cur_dsv, (unsigned long long)n);
    }
    if (d.depth.st) {                            // the patterns DRAWN, each once: states created are not states used
        ++g_st_emis;
        static uint64_t vus[32][2];
        static int nvus = 0;
        const uint64_t k0 = ((uint64_t)d.depth.st << 32) | d.depth.st_ops,
                       k1 = ((uint64_t)(d.depth.enable & 1) << 33) | ((uint64_t)(d.depth.write & 1) << 32) | d.depth.func;
        int i = 0;
        while (i < nvus && (vus[i][0] != k0 || vus[i][1] != k1)) i++;
        if (i == nvus && nvus < 32) {
            vus[nvus][0] = k0; vus[nvus++][1] = k1;
            const uint32_t o = d.depth.st_ops;
            fprintf(stderr, "[POCHOIR] motif DESSINE #%d : lecture=%02x ecriture=%02x avant F/Z/P/f=%u/%u/%u/%u "
                            "arriere=%u/%u/%u/%u profondeur=%u/%u/%u ref=%u cull=%u plan=%08x rtv=%08x\n", nvus, (d.depth.st >> 8) & 0xFF,
                    d.depth.st >> 16, o & 15, (o >> 4) & 15, (o >> 8) & 15, (o >> 12) & 15, (o >> 16) & 15,
                    (o >> 20) & 15, (o >> 24) & 15, o >> 28, d.depth.enable, d.depth.write, d.depth.func,
                    d.depth.st_ref, g_cur_raster.cull, d.depth.st_res, g_cur_rtv);
        }
    }
    if (g_ciseau && g_cur_raster.ciseau) {
        d.rs = 1;
        for (int i = 0; i < 4; i++) d.sc[i] = g_sc_now[i];
        static uint64_t n = 0;                   // D3D11 would draw nothing here either: its rectangle is empty
        if (!g_sc_pose && puiss2(++n))
            fprintf(stderr, "[CISEAU] dessins a ScissorEnable=1 AVANT tout RSSetScissorRects : %llu\n",
                    (unsigned long long)n);
    }
    if (g_depth_bias) {
        d.zb = g_cur_raster.biais; d.zb_pente = g_cur_raster.pente; d.zb_max = g_cur_raster.biais_max;
        if (g_cur_raster.pince_z) d.rs |= 4;
    }
    if (g_cull) {
        d.rs |= (uint32_t)(g_cur_raster.cull & 3) << 8;
    } else if (d.depth.st && g_cur_raster.cull >= 2) {
        // Without CullMode, the faces D3D11 would have culled still run their stencil ops (the game copies the
        // front ops into the back face). Harmless for LUI's quads; wrong for an INCR/DECR/INVERT 3D material.
        static uint64_t n = 0, n_ops = 0;
        for (int i = 0; i < 3; i++) {
            const uint32_t o = (d.depth.st_ops >> (4 * i)) & 15;
            if (o != 1 && o != 3) { ++n_ops; break; }
        }
        if (puiss2(++n))
            fprintf(stderr, "[POCHOIR] dessins a pochoir sous CullMode FRONT/BACK non traduit : %llu "
                            "(dont ops hors KEEP/REPLACE : %llu)\n", (unsigned long long)n, (unsigned long long)n_ops);
    }
    // WHAT IS ACTUALLY BOUND TO baseLut2D? The comment on BO2_UI_NOLUTGEN says
    // the base LUT is the 1x1 white placeholder "with no images loaded" — but the
    // run now loads 1742 images, so that premise needs re-checking rather than
    // inheriting. Skipping the generator resolves the composite to BLACK and
    // keeping it resolves it to WHITE, so neither is right: the LUT itself has to
    // become correct, and that starts with knowing which texture feeds it.
    // THE FOG VALUES, by NAME and at the translator's own offsets. The floor and
    // the far wall resolve to pure fog colour when the factor is 0, so these six
    // vectors decide whether the washout that is left is a fog bug at all.
    if (bo2_getenv("BO2_FOG_PROBE") && g_fog_vs.count(g_cur_vs)) {
        // ⚠️ A RUN-WIDE CAP OF 3 REPORTED THE TITLE SCREEN'S SHADERS AND NOTHING
        // ELSE — the first three fog draws of the run are 2D UI, and their fog
        // constants are legitimately zero. Reading "the fog constants are zero"
        // off that would have been the same capped-view mistake this project has
        // now made seven times. Count PER VERTEX SHADER instead, so every fog
        // shader that ever draws reports once, whenever it first draws.
        // 🪤 ...AND PER-SHADER "FIRST TWO DRAWS" WAS STILL A CAPPED VIEW. On the
        // campaign it reported PerSceneConsts as 5/612 floats non-zero, which is
        // impossible for a buffer the very same shader reads viewProjectionMatrix
        // out of: at 5/612 the matrix is zero and every vertex collapses to the
        // origin, yet afghanistan renders with a moving camera. The first draws of
        // a shader are simply not its steady state. Report the first two AND a
        // periodic LATE sample, so the run says what the buffer looks like while
        // the map is actually being drawn.
        static std::unordered_map<uint32_t, uint32_t> nfby;
        uint32_t nf = ++nfby[g_cur_vs];
        if (nf <= 2 || (nf % 20000) == 0) {
            uint32_t reg = gfx_cbreg_for_mtl(g_cur_vs, 0);   // PerSceneConsts = buffer(0)
            // WHICH IT WAS. Falling back to b0 on a failed lookup and printing
            // "PerSceneConsts <- d3d b0" reads identically to a successful
            // resolution, so a mapping bug and a genuinely-empty buffer were
            // indistinguishable in the log.
            const bool reg_mapped = (reg != 0xffffffffu);
            if (reg == 0xffffffffu) reg = 0;                 // no map = identity
            const auto& lay = g_cb_layout[g_cur_vs];
            const uint8_t* cb = (reg < kMaxCB) ? (const uint8_t*)d.vscb[reg] : nullptr;
            uint32_t nb = (reg < kMaxCB) ? d.vscb_bytes[reg] : 0;
            // How much of the buffer is live at all? "These six vectors are zero"
            // means something completely different depending on whether the rest
            // of the buffer is populated.
            uint32_t nz = 0;
            if (cb) for (uint32_t q = 0; q + 4 <= nb; q += 4) {
                float v; memcpy(&v, cb + q, 4);
                if (v != 0.0f) nz++;
            }
            // viewProjectionMatrix is the honesty check: this shader multiplies
            // every vertex by it, so if it reads as zero here while the map is
            // visibly on screen, we are reading the WRONG BUFFER and every other
            // number on this line is meaningless.
            float vp0[4] = {0,0,0,0};
            auto vpo = lay.find("viewProjectionMatrix");
            bool vp_known = (vpo != lay.end() && cb && vpo->second + 16 <= nb);
            if (vp_known) memcpy(vp0, cb + vpo->second, 16);
            fprintf(stderr, "[FOG] draw #%u vs=%08x ps=%08x  PerSceneConsts <- d3d b%u%s "
                            "(%u bytes, %u/%u floats non-zero) viewProj[0]=%s%.3f %.3f %.3f %.3f\n",
                    nf, g_cur_vs, g_cur_ps, reg,
                    reg_mapped ? "" : " (UNMAPPED, defaulted)", nb, nz, nb / 4,
                    vp_known ? "" : "(not in layout) ",
                    vp0[0], vp0[1], vp0[2], vp0[3]);
            // WHERE IS THE CAMERA, in world units. "we are not in the right place"
            // is an impression until this prints a number: inverseViewMatrix's
            // fourth row is the camera's world position and eyeOffset is CoD's own
            // eye origin, so a reading near (0,0,0) means the player was never
            // placed at the map's spawn at all — which needs no knowledge of what
            // afghanistan's correct spawn coordinates are to conclude.
            {
                auto ivm = lay.find("inverseViewMatrix");
                if (ivm != lay.end() && cb && ivm->second + 64 <= nb) {
                    float m[16]; memcpy(m, cb + ivm->second, 64);
                    fprintf(stderr, "[CAM] inverseViewMatrix row3 (world pos) = "
                                    "%.1f %.1f %.1f %.1f\n", m[12], m[13], m[14], m[15]);
                }
                // ...AND THE SAME POSITION DERIVED FROM viewProjectionMatrix, which
                // is definitely populated (the shaders transform every vertex with
                // it). inverseViewMatrix reading zero is ambiguous — the engine may
                // simply not fill it for these shaders — so this is the independent
                // check. The shader computes clip.i = dot(worldPos, VP[i]), so the
                // eye is the world point where clip.x, clip.y and clip.w all vanish:
                //   dot(p, VP[i].xyz) + VP[i].w = 0   for i in {0, 1, 3}
                // Three equations, three unknowns, solved by Cramer.
                auto vpm = lay.find("viewProjectionMatrix");
                if (vpm != lay.end() && cb && vpm->second + 64 <= nb) {
                    float M[16]; memcpy(M, cb + vpm->second, 64);
                    const int ri[3] = { 0, 1, 3 };
                    double A[3][3], B[3];
                    for (int i = 0; i < 3; i++) {
                        A[i][0] = M[ri[i]*4+0]; A[i][1] = M[ri[i]*4+1];
                        A[i][2] = M[ri[i]*4+2]; B[i]    = -M[ri[i]*4+3];
                    }
                    auto det3 = [](double a0,double a1,double a2,double b0,double b1,
                                   double b2,double c0,double c1,double c2) {
                        return a0*(b1*c2-b2*c1) - a1*(b0*c2-b2*c0) + a2*(b0*c1-b1*c0);
                    };
                    double D = det3(A[0][0],A[0][1],A[0][2], A[1][0],A[1][1],A[1][2],
                                    A[2][0],A[2][1],A[2][2]);
                    if (D > 1e-12 || D < -1e-12) {
                        double x = det3(B[0],A[0][1],A[0][2], B[1],A[1][1],A[1][2],
                                        B[2],A[2][1],A[2][2]) / D;
                        double y = det3(A[0][0],B[0],A[0][2], A[1][0],B[1],A[1][2],
                                        A[2][0],B[2],A[2][2]) / D;
                        double z = det3(A[0][0],A[0][1],B[0], A[1][0],A[1][1],B[1],
                                        A[2][0],A[2][1],B[2]) / D;
                        fprintf(stderr, "[CAM] eye from viewProj = %.1f %.1f %.1f\n",
                                x, y, z);
                    } else {
                        fprintf(stderr, "[CAM] eye from viewProj: SINGULAR (det=%.3g) — "
                                        "this matrix is not a perspective transform\n", D);
                    }
                }
                for (const char* c : { "eyeOffset", "cameraLook", "cameraUp" }) {
                    auto o = lay.find(c);
                    if (o == lay.end() || !cb || o->second + 16 > nb) continue;
                    float v[4]; memcpy(v, cb + o->second, 16);
                    fprintf(stderr, "[CAM]   %-12s %.2f %.2f %.2f %.2f\n",
                            c, v[0], v[1], v[2], v[3]);
                }
            }
            static const char* want[] = { "fogColor", "fogConsts", "fogConsts2",
                                          "sunFogColor", "sunFog", "hdrControl0" };
            for (const char* w : want) {
                auto o = lay.find(w);
                if (o == lay.end()) { fprintf(stderr, "[FOG]   %-12s not in layout\n", w); continue; }
                if (!cb || o->second + 16 > nb) {
                    fprintf(stderr, "[FOG]   %-12s @%u  OUT OF RANGE (buffer %u B)\n",
                            w, o->second, nb);
                    continue;
                }
                float f[4]; memcpy(f, cb + o->second, 16);
                fprintf(stderr, "[FOG]   %-12s @%-4u %9.4f %9.4f %9.4f %9.4f\n",
                        w, o->second, f[0], f[1], f[2], f[3]);
            }
        }
    }
    // REMEMBER WHERE THE LUT LANDS, on every generator draw and unconditionally:
    // the consumer probe below has to compare against it, and a value that only
    // exists when a probe is on is a value the probe cannot check.
    if (g_lutgen_ps.count(g_cur_ps)) {
        auto rv = g_rtv_res.find(g_cur_rtv);
        if (rv != g_rtv_res.end() && rv->second) g_lut_rt_res = rv->second;
    }
    // THE CONSUMER SIDE. The generator is exonerated end to end, so the question
    // is now exactly: does the resource that reaches the composite's LUT sampler
    // equal the target the generator wrote? Print the WHOLE table — Metal index,
    // the declared sampler name at that index, the D3D register it was fed from,
    // the resource and its dimensions — because the failure mode this shape
    // invites is a PERMUTATION, and a permutation is invisible one slot at a time.
    if (g_lutuse_ps.count(g_cur_ps)) {
        // Run-wide caps of 4 and 6 are early-draw views, and this session has
        // already been burned twice by exactly that (the fog probe reported the
        // title screen's shaders, then reported "5/612 floats non-zero" off a
        // shader's first two draws while the map rendered fine). The frontend
        // draws this composite too, so the first four reports can be entirely
        // menu. Sample the first few AND periodically thereafter.
        static uint32_t nu = 0;
        ++nu;
        if (bo2_getenv("BO2_LUT_PROBE") && (nu <= 4 || (nu % 3000) == 0)) {
            fprintf(stderr, "[LUT-USE] composite draw #%u ps=%08x  (generator wrote "
                            "into res=%08x)\n", nu, g_cur_ps, g_lut_rt_res);
            const std::vector<std::string>& nm = g_tex_names[g_cur_ps];
            uint32_t mtl = 0, d3d = 0;
            int any = 0;
            for (int i = 0; gfx_texslot_at(g_cur_ps, i, &mtl, &d3d); i++) {
                any = 1;
                uint32_t res = (d3d < kMaxPSTex) ? d.pstex[d3d] : 0;
                uint32_t w = 0, h = 0;
                int known = gfx_texture_dims(res, &w, &h);
                fprintf(stderr, "[LUT-USE]   mtl%u '%s' <- d3d t%u = res %08x %s%ux%u%s\n",
                        mtl, mtl < nm.size() ? nm[mtl].c_str() : "?", d3d, res,
                        known ? "" : "UNKNOWN ", w, h,
                        (res && res == g_lut_rt_res) ? "   <== THE GENERATED LUT" : "");
            }
            if (!any)
                fprintf(stderr, "[LUT-USE]   no texture map recorded for this shader "
                                "— every slot binds IDENTITY\n");
        }
    }
    if (g_lutgen_ps.count(g_cur_ps)) {
        static uint32_t nl = 0;
        ++nl;
        if (bo2_getenv("BO2_LUT_PROBE") && (nl <= 6 || (nl % 3000) == 0)) {
            uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
            int k0 = gfx_texture_dims(d.pstex[0], &w0, &h0);
            int k1 = gfx_texture_dims(d.pstex[1], &w1, &h1);
            fprintf(stderr, "[LUT-IN] generator draw #%u ps=%08x baseLut2D=%08x %s"
                            "%ux%u | slot1=%08x %s%ux%u (idx=%u)\n",
                    nl, g_cur_ps, d.pstex[0], k0 ? "" : "UNKNOWN ", w0, h0,
                    d.pstex[1], k1 ? "" : "UNKNOWN ", w1, h1, d.idx_count);
            // AND THE SLOT MAP. HLSLcc renumbers constant-buffer indices and the
            // backend carries a per-shader map for it; a shader with NO map is
            // bound identity. The generator wants PerSceneConsts at MSL
            // buffer(0), so if the guest does not put PerSceneConsts in slot 0
            // an identity binding feeds it whatever slot 0 holds — and slot 0
            // measured all zero. Print the map next to the data so the two can be
            // compared in one line instead of across two runs.
            {
                const char* m = gfx_cbmap_for(g_cur_ps);
                // ...AND WHERE THE RESULT GOES. The maths is now accounted for
                // (real LUT in, live constants, identity colour matrix), so the
                // remaining question is the OUTPUT: which render target this draw
                // writes, whether we know that target as a texture at all, and
                // therefore whether the composite can ever sample the LUT it
                // produced. A generator that computes correctly into a target
                // nobody can read looks exactly like a broken generator.
                uint32_t rw = 0, rh = 0;
                // Resolve the bound RTV to its resource the same way the draw path
                // does; a view is not a texture and the registry is keyed by
                // resource.
                uint32_t rres = 0;
                { auto rv = g_rtv_res.find(g_cur_rtv);
                  if (rv != g_rtv_res.end()) rres = rv->second; }
                int known = gfx_texture_dims(rres, &rw, &rh);
                fprintf(stderr, "[LUT-MAP] ps=%08x cbmap=\"%s\" | rtv=%08x res=%08x "
                                "%s%ux%u | backbuffer_rtv=%08x\n",
                        g_cur_ps, m ? m : "(none — identity binding)",
                        g_cur_rtv, rres, known ? "" : "NOT A KNOWN TEXTURE ",
                        rw, rh, g_backbuffer_rtv);
            }
            // THE CONSTANTS, because the texture turned out to be innocent. The
            // generator computes sample(baseLut2D)*postFxControlE.y +
            // sample(...)*postFxControlE.x, then a BT.709 luma and
            // postFxControl0/1/2. A WHITE result is a saturating multiply, not a
            // zero — so the value of these decides where to look next, and
            // guessing at a post-process chain is how a day disappears.
            for (int cb = 0; cb < kMaxCB; cb++) {
                if (!d.pscb[cb] || d.pscb_bytes[cb] < 16) continue;
                const float* f = (const float*)d.pscb[cb];
                const uint32_t nfl = d.pscb_bytes[cb] / 4;
                // ⚠️ THE FIRST 24 FLOATS ARE NOT THE BUFFER. I read "all zero"
                // off exactly that and nearly chased a binding bug that is not
                // there (the map says mtl0<-d3d0, which is correct): the struct
                // opens with windDirection / variantWindSpring[16], legitimately
                // zero in a menu, and postFxControl* live far down it. Same trap
                // as every capped log in this project. And computing the offset
                // by hand is the OTHER trap — HLSL cbuffer packing is not
                // sequential. So print the NON-ZERO RUNS of the whole buffer and
                // let the data show where the live values are.
                fprintf(stderr, "[LUT-CB] ps cb%d: %u floats, non-zero runs:", cb, nfl);
                uint32_t shown = 0;
                for (uint32_t q = 0; q < nfl && shown < 40; q++) {
                    if (f[q] == 0.0f) continue;
                    uint32_t r = q;
                    while (r + 1 < nfl && f[r + 1] != 0.0f) r++;
                    fprintf(stderr, "  [%u..%u]", q, r);
                    for (uint32_t t = q; t <= r && t < q + 8; t++)
                        fprintf(stderr, " %.4f", f[t]);
                    shown++;
                    q = r;
                }
                fprintf(stderr, "\n");
            }
        }
    }
    static const int nolutgen = bo2_getenv("BO2_UI_NOLUTGEN") ? 1 : 0;
    if (nolutgen && g_lutgen_ps.count(g_cur_ps)) {
        static uint32_t nskip = 0;
        if (++nskip == 1)
            fprintf(stderr, "[LUT] BO2_UI_NOLUTGEN: skipping the LUT generator so the "
                            "composite resolves to black instead of placeholder white\n");
        return;
    }
    gfx_real_draw(&d);
}

enum { IF_USER = 1, IF_UTILS = 2, IF_USERSTATS = 3, IF_APPS = 4, IF_FRIENDS = 5, IF_NETWORKING = 6, IF_GAMESERVER = 7 };
static constexpr uint32_t STEAM_VTBL_BASE  = 0xE7000000u;
static constexpr int      STEAM_VTBL_SLOTS = 128;

// Lazily build (once) and return a fake interface object for `iface_id`.
static uint32_t steam_iface_obj(int iface_id) {
    static uint32_t objs[16] = {0};
    if (iface_id < 1 || iface_id >= 16) return 0;
    if (objs[iface_id]) return objs[iface_id];
    uint32_t vtbl = win32_guest_alloc(STEAM_VTBL_SLOTS * 4, 4);
    for (int i = 0; i < STEAM_VTBL_SLOTS; i++)
        win32_mem_write32(vtbl + i * 4, STEAM_VTBL_BASE | (uint32_t)(iface_id << 8) | (uint32_t)i);
    uint32_t obj = win32_guest_alloc(8, 4);
    win32_mem_write32(obj + 0, vtbl);
    win32_mem_write32(obj + 4, 0);
    objs[iface_id] = obj;
    return obj;
}

static void impl_SteamUser(State& s)      { s.gpr.rax.dword = steam_iface_obj(IF_USER);      win32_cdecl_ret(s); }
static void impl_SteamUtils(State& s)     { s.gpr.rax.dword = steam_iface_obj(IF_UTILS);     win32_cdecl_ret(s); }
static void impl_SteamUserStats(State& s) { s.gpr.rax.dword = steam_iface_obj(IF_USERSTATS); win32_cdecl_ret(s); }
static void impl_SteamApps(State& s)      { s.gpr.rax.dword = steam_iface_obj(IF_APPS);      win32_cdecl_ret(s); }
static void impl_SteamFriends(State& s)   { s.gpr.rax.dword = steam_iface_obj(IF_FRIENDS);   win32_cdecl_ret(s); }
// t6zm only (t6sp does not import it). Null, the per-frame interface validation 0x5ec620 never cleared its pending
// bit 0x20 at 0xd54290 and called the accessor every frame (x22631 in 180 s, run of 08/10).
static void impl_SteamNetworking(State& s) { s.gpr.rax.dword = steam_iface_obj(IF_NETWORKING); win32_cdecl_ret(s); }
// t6zm only. `map` starts the local server through the Steam game server (0x540145): unimplemented, Init answered 0
// and the load ended on Com_Error "PLATFORM_COULDNT_INIT_STEAM_SERVER" (run of 08/10). Answers of
// Steam on Windows: SteamGameServer_Init returns true once the server is up,
// SteamGameServer() the interface the game asks for, version 011 (Z-NATIF-09p).
static void impl_SteamGameServer_Init(State& s) { s.gpr.rax.dword = 1; win32_cdecl_ret(s); }
static void impl_SteamGameServer(State& s) { s.gpr.rax.dword = steam_iface_obj(IF_GAMESERVER); win32_cdecl_ret(s); }
// Nothing is queued for the game server; the server thread calls RunCallbacks every frame.
BINK_STUB(SteamGameServer_RunCallbacks)
BINK_STUB(SteamGameServer_Shutdown)

// Decode a Steam vtable-method sentinel and answer it. Returns false if `addr`
// is not a Steam sentinel (caller continues normal dispatch).
extern "C" bool steam_vtbl_dispatch(State& s, uint32_t addr) {
    if ((addr & 0xFF000000u) != STEAM_VTBL_BASE) return false;
    int iface = (int)((addr >> 8) & 0xFF);
    int slot  = (int)(addr & 0xFF);
    uint32_t esp = s.gpr.rsp.dword;
    // thiscall: callee cleans args; return `rax` and pop retaddr + nargs*4.
    auto ret = [&](uint32_t rax, int nargs) {
        s.gpr.rax.dword = rax;
        s.gpr.rsp.dword = esp + 4 + (uint32_t)nargs * 4;
    };

    // ISteamUser::BLoggedOn() (slot 1): the player's Steam answers. 0 args.
    if (iface == IF_USER && slot == 1) { const SteamReel& r = steam_reel(); ret(r.logged(r.user), 0); return true; }

    // Steam réel : l'identité, l'abonnement et les DLC viennent de Steam (ISteamApps : 0 BIsSubscribed,
    // 6 BIsSubscribedApp(appid), 7 BIsDlcInstalled(appid)).
    {
        const SteamReel& r = steam_reel();
        if ((iface == IF_USER && slot == 2) || (iface == IF_GAMESERVER && slot == 10)) {
            const uint32_t out = win32_mem_read32(esp + 4);
            win32_mem_write32(out + 0, (uint32_t)r.id);
            win32_mem_write32(out + 4, (uint32_t)(r.id >> 32));
            ret(out, 1);
            return true;
        }
        if (iface == IF_APPS && slot == 0) { ret(r.sub(r.apps, kSteamAppId), 0); return true; }
        if (iface == IF_APPS && slot == 6) { ret(r.sub(r.apps, win32_mem_read32(esp + 4)), 1); return true; }
        if (iface == IF_APPS && slot == 7) { ret(r.dlc(r.apps, win32_mem_read32(esp + 4)), 1); return true; }
    }

    // Arity (the ABI of the interface) of every method, from Valve's public Steamworks headers by
    // tools/steam/steam_arity.py. Unanswered, a method's arguments stayed on the stack and the caller returned to
    // them: ISteamApps::BIsDlcInstalled (slot 7), then BIsSubscribedApp (slot 6).
    #include "steam_arity.inc"
    // ISteamGameServer011::InitGameServer (slot 0, 6 arguments): true, the local server starts. GetSteamID (slot 10)
    // is the player's SteamID, answered above.
    if (iface == IF_GAMESERVER && slot == 0) { ret(1, 6); return true; }
    // ISteamUserStats011: RequestCurrentStats (slot 0) is false only when no user is logged on (Steamworks
    // documentation); StoreStats (slot 10) is asked to the player's Steam.
    if (iface == IF_USERSTATS && slot == 0) { const SteamReel& r = steam_reel(); ret(r.logged(r.user), 0); return true; }
    if (iface == IF_USERSTATS && slot == 10) { const SteamReel& r = steam_reel(); ret(r.store(r.stats), 0); return true; }
    if (iface == IF_FRIENDS && slot == 0) { ret(steam_persona_name(), 0); return true; }   // GetPersonaName
    if (iface == IF_UTILS && slot == 9) { ret(g_steam_appid, 0); return true; }       // GetAppID
    // ISteamUser016::RequestEncryptedAppTicket (slot 20) and GetEncryptedAppTicket (slot 21): see g_steam_app_ticket.
    if (iface == IF_USER && slot == 20) {
        const uint32_t data = win32_mem_read32(esp + 4), size = win32_mem_read32(esp + 8);
        std::lock_guard<std::mutex> lock(g_steam_mu);
        memset(g_steam_app_ticket, 0, sizeof g_steam_app_ticket);
        for (uint32_t i = 0; i < size && i < sizeof g_steam_app_ticket; i++) g_steam_app_ticket[i] = win32_mem_read8(data + i);
        g_steam_ticket_len = size < sizeof g_steam_app_ticket ? size : sizeof g_steam_app_ticket;
        const uint64_t call = ++g_steam_calls;
        g_steam_done.push_back(call);
        fprintf(stderr, "[STEAM] RequestEncryptedAppTicket(%u octets) ret=0x%08x -> appel %llu\n", size,
                win32_mem_read32(esp), (unsigned long long)call);
        s.gpr.rdx.dword = (uint32_t)(call >> 32);
        ret((uint32_t)call, 2);
        return true;
    }
    if (iface == IF_USER && slot == 21) {
        const uint32_t out = win32_mem_read32(esp + 4), max = win32_mem_read32(esp + 8), size = win32_mem_read32(esp + 12);
        std::lock_guard<std::mutex> lock(g_steam_mu);
        for (uint32_t i = 0; i < max && i < sizeof g_steam_app_ticket; i++) win32_mem_write8(out + i, g_steam_app_ticket[i]);
        win32_mem_write32(size, g_steam_ticket_len);
        fprintf(stderr, "[STEAM] GetEncryptedAppTicket(max %u) ret=0x%08x -> %u octets\n", max, win32_mem_read32(esp), g_steam_ticket_len);
        ret(1, 3);
        return true;
    }
    // Any other method of the table: 0, its arguments popped. [STEAM-UNIMPL] names the first call of each site.
    if (iface < (int)(sizeof kSteamArity / sizeof kSteamArity[0]) && slot < kSteamArity[iface].n) {
        const int args = kSteamArity[iface].a[slot];
        static DiagCounter<8> answered;   // without a lock, as below
        if (answered.bump(addr) == 0)
            fprintf(stderr, "[STEAM-UNIMPL] iface=%d slot=%d args=%d ret=0x%08x this=0x%08x -> 0\n", iface, slot,
                    args, win32_mem_read32(esp), s.gpr.rcx.dword);
        ret(0, args);
        return true;
    }

    // Unimplemented: log loudly (iface/slot/caller) and return 0. Cleans 0 args
    // (may leak a thiscall arg) — fix by adding a handler with the real arity
    // the moment this fires. This is the measure -> implement loop.
    static DiagCounter<8> seen;   // P661/C15 : sans verrou (appel possible depuis un fil heberge)
    if (seen.bump(addr) == 0)
        fprintf(stderr, "[STEAM-UNIMPL] iface=%d slot=%d ret=0x%08x this=0x%08x -> 0\n",
                iface, slot, win32_mem_read32(esp), s.gpr.rcx.dword);
    ret(0, 0);
    return true;
}

extern "C" uint32_t win32_shim_alloc(uint32_t size, uint32_t align);
// --- Shared fake-COM machinery for the audio interfaces (0xE5iissss) --------
// Hoisted ABOVE ole32 on purpose: CoCreateInstance (ole32) mints the IXAudio2
// object, and in C++ that use has to come after the declaration. Interface ids
// 1-4 are DirectSound, 5+ are XAudio2; both share one sentinel space and one
// object allocator so there is a single place that knows how a fake COM object
// is laid out.
enum { DS_IF_DEVICE = 1, DS_IF_BUFFER = 2, DS_IF_CAPTURE = 3, DS_IF_NOTIFY = 4 };
static constexpr uint32_t DS_VTBL_BASE  = 0xE5000000u;
static constexpr int      DS_VTBL_SLOTS = 64;
// A FRESH object sharing the interface's vtable. Voices must NOT be a cached
// singleton: the engine keeps an array of source voices and tracks them BY
// POINTER, so handing out one shared object makes every voice look like the same
// voice and its bookkeeping collapses — later Start/SubmitSourceBuffer calls
// find a slot it believes is already taken and reach through a null. This is the
// same singleton bug as ID3D11View::GetResource, CreateTexture1D/3D and
// GetForegroundWindow: wherever the shim hands back one object for many, the
// guest cannot tell them apart.
static uint32_t ds_iface_new(int id);
static uint32_t ds_iface_obj(int id) {
    static uint32_t objs[16] = {0};
    if (id < 1 || id >= 16) return 0;
    if (objs[id]) return objs[id];
    uint32_t vtbl = win32_shim_alloc(DS_VTBL_SLOTS * 4, 4);
    for (int i = 0; i < DS_VTBL_SLOTS; i++)
        win32_mem_write32(vtbl + i * 4, DS_VTBL_BASE | (uint32_t)(id << 8) | (uint32_t)i);
    uint32_t obj = win32_shim_alloc(64, 4);
    win32_mem_write32(obj + 0, vtbl);
    for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
    objs[id] = obj;
    return obj;
}
static uint32_t ds_iface_new(int id) {
    if (id < 1 || id >= 16) return 0;
    uint32_t vtbl = win32_shim_alloc(DS_VTBL_SLOTS * 4, 4);
    for (int i = 0; i < DS_VTBL_SLOTS; i++)
        win32_mem_write32(vtbl + i * 4, DS_VTBL_BASE | (uint32_t)(id << 8) | (uint32_t)i);
    uint32_t obj = win32_shim_alloc(64, 4);
    win32_mem_write32(obj + 0, vtbl);
    for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
    return obj;
}

// --- ole32 (5 functions) ---
// StringFromGUID2(rguid, lpsz, cchMax) — stdcall 3 : ecrit "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" en UTF-16 et
// rend 39 (nul compris), 0 si le tampon est trop court. Etait un talon a UN argument qui rendait 1 sans rien
// ecrire : esp restait 8 octets trop bas chez l'appelant (t6zm 0x5b8f80, controle de la classe CLSID\{F2FB7BAF-…}
// a la creation du salon, les lundis, mercredis et vendredis UTC) ; les appelants lisaient leurs arguments decales
// et le jeu finissait dans atoi(NULL) -> TerminateProcess(0xC0000417) (Z-NATIF-13d-bis).
static void impl_StringFromGUID2(State& s) {
    const uint32_t g = win32_arg(s, 0), out = win32_arg(s, 1), max = win32_arg(s, 2);
    char t[40];
    snprintf(t, sizeof t, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", win32_mem_read32(g),
             win32_mem_read32(g + 4) & 0xffffu, win32_mem_read32(g + 4) >> 16, win32_mem_read8(g + 8),
             win32_mem_read8(g + 9), win32_mem_read8(g + 10), win32_mem_read8(g + 11), win32_mem_read8(g + 12),
             win32_mem_read8(g + 13), win32_mem_read8(g + 14), win32_mem_read8(g + 15));
    const uint32_t n = max >= 39u ? 39u : 0u;
    for (uint32_t k = 0; k < n; k++) win32_mem_write16(out + 2u * k, (uint16_t)t[k]);   // t[38] = le nul
    fprintf(stderr, "[OLE] StringFromGUID2 %s cchMax=%u -> %u\n", t, max, n);
    s.gpr.rax.dword = n;
    win32_stdcall_ret(s, 3);
}
// CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv) — stdcall 5
// (was DLL_STUB4 -> 4B ESP leak/call). Return REGDB_E_CLASSNOTREG with *ppv=0:
// the XAudio2 sound init (FUN_00885cb0) `jl`-checks the HRESULT and exits its
// no-sound path cleanly; the old S_OK-with-garbage-out made it run virtual
// calls on uninitialized stack ([NULL-CALL] @0x885ebd/0x495db0).
//
// BO2_XAUDIO=1 answers with a fake IXAudio2 instead. THE AUDIO BACKEND IS
// XAUDIO2, NOT DIRECTSOUND — DSOUND.dll is imported but only as a fallback; the
// real device comes through this CoCreateInstance, and the exe carries the RTTI
// to prove it (.?AUIXAudio2EngineCallback@@, .?AVSD_XAudio2Callbacks@@) plus the
// `sd_xa2_num_devices` dvar. Why it matters beyond sound: FUN_00885ed0 returns
// FALSE while [0xe96df8] is 0, so FUN_005e1800 writes [0x300d92c]=0xffffffff,
// so FUN_005927c0 bails BEFORE FUN_00542aa0 — the Sound Mix thread is never
// created, the mixer tick never runs, and FUN_008857c0 (the VIDEO subsystem's
// audio callback) never pumps either. One refusal disables two subsystems.
//
// ARITIES ARE NOT OPTIONAL. These are stdcall COM methods: a wrong argument
// count displaces the guest stack by 4x the error and the corruption surfaces
// frames later somewhere unrelated. The table below is IXAudio2 (XAudio2 2.7)
// in vtable order, and every entry carries its real count.
extern "C" void bo2_audio_set_format(int channels, int rate, int bits);
extern "C" void bo2_audio_submit(const void* pcm, uint32_t bytes);
extern "C" void bo2_audio_state(uint32_t* queued, uint64_t* samples);
extern "C" uint32_t bo2_audio_take_completed(uint32_t unit_bytes, uint32_t max);
extern "C" bool win32_run_lifted(State* s, uint32_t va);
static uint32_t g_xa_voice_cb = 0;      // guest IXAudio2VoiceCallback*
// Every voice object we minted, so we can tell WHEN its vtable word is clobbered.
// The pointer stays valid while [obj+0] goes to 0, which is the signature of
// something writing over our allocation rather than of a stale pointer.
// Voice objects live in the SHIM ARENA (win32_shim_alloc), disjoint from the
// guest heap. They used to be bump-allocated alongside the game's own blocks
// and had their vtable pointer zeroed by a neighbouring guest allocation
// written past its end — the re-arm workaround that used to sit here is gone
// because separating the arenas fixed the cause (re-arms 0, [NULL-CALL] 0).
static uint32_t g_xa_unit     = 0;      // bytes per submission
enum { XA_IF_ENGINE = 5, XA_IF_MASTER = 6, XA_IF_SOURCE = 7, XA_IF_SUBMIX = 8 };
// ⚠️ SLOT 7 IS UnregisterForCallbacks AND IT IS EASY TO FORGET. Leaving it out
// shifts every factory down one: I had CreateMasteringVoice at 9, the engine
// calls it at 10, so I answered it as StartEngine — never filled the
// out-parameter (NULL mastering voice, which then produced a [NULL-CALL] on
// DestroyVoice at 0x885c23) AND popped 1 argument instead of 7. Verified against
// what the engine actually does: the call at 0x885c0d pushes ~7 values, which is
// CreateMasteringVoice, not a no-arg StartEngine.
static const uint8_t kXA2Args[16] = {
    3, 1, 1,   // 0 QueryInterface, 1 AddRef, 2 Release
    2,         // 3 GetDeviceCount(this,*count)
    3,         // 4 GetDeviceDetails(this,idx,*details)
    3,         // 5 Initialize(this,flags,processor)
    2,         // 6 RegisterForCallbacks(this,cb)
    2,         // 7 UnregisterForCallbacks(this,cb)
    8,         // 8 CreateSourceVoice
    8,         // 9 CreateSubmixVoice
    7,         // 10 CreateMasteringVoice
    1, 1,      // 11 StartEngine, 12 StopEngine
    2,         // 13 CommitChanges(this,opset)
    2,         // 14 GetPerformanceData(this,*perf)
    3,         // 15 SetDebugConfiguration(this,pDebug,pReserved)
};
// IXAudio2Voice / IXAudio2SourceVoice, in vtable order. Voices are NOT
// IUnknown-derived, so this table starts at GetVoiceDetails, not QueryInterface.
static const uint8_t kXAVoiceArgs[29] = {
    2,   // 0  GetVoiceDetails(this,*p)
    3,   // 1  SetOutputVoices(this,pSends)          [+opset on some builds]
    2,   // 2  SetEffectChain(this,pChain)
    3,   // 3  EnableEffect(this,idx,opset)
    3,   // 4  DisableEffect(this,idx,opset)
    3,   // 5  GetEffectState(this,idx,*enabled)
    5,   // 6  SetEffectParameters(this,idx,params,size,opset)
    4,   // 7  GetEffectParameters(this,idx,params,size)
    3,   // 8  SetFilterParameters(this,pParams,opset)
    2,   // 9  GetFilterParameters(this,pParams)
    4,   // 10 SetOutputFilterParameters(this,pDest,pParams,opset)
    3,   // 11 GetOutputFilterParameters(this,pDest,pParams)
    3,   // 12 SetVolume(this,volume,opset)
    2,   // 13 GetVolume(this,*volume)
    4,   // 14 SetChannelVolumes(this,ch,pVolumes,opset)
    3,   // 15 GetChannelVolumes(this,ch,pVolumes)
    6,   // 16 SetOutputMatrix(this,pDest,src,dst,pLevels,opset)
    5,   // 17 GetOutputMatrix(this,pDest,src,dst,pLevels)
    1,   // 18 DestroyVoice(this)
    3,   // 19 Start(this,flags,opset)
    3,   // 20 Stop(this,flags,opset)
    3,   // 21 SubmitSourceBuffer(this,pBuffer,pBufferWMA)
    1,   // 22 FlushSourceBuffers(this)
    1,   // 23 Discontinuity(this)
    2,   // 24 ExitLoop(this,opset)
    2,   // 25 GetState(this,*state)
    3,   // 26 SetFrequencyRatio(this,ratio,opset)
    2,   // 27 GetFrequencyRatio(this,*ratio)
    2,   // 28 SetSourceSampleRate(this,rate)
};
// Flush finished-buffer callbacks. MUST run on a guest thread, and must NOT
// depend on the guest calling a voice method: BO2 queues 6 buffers and then
// waits for OnBufferEnd before submitting again, so hooking the flush to voice
// calls deadlocks — the thing that would wake it up only fires if it is already
// awake. Called from the per-frame tick instead.
// Sound Mix loop sampler (BO2_AUDIO_LOG). Reads [0xbf1884] (the "do the work at
// all" gate), [0xbf188c] (the iteration counter) and [0xbf1888] straight out of
// guest memory, so it cannot perturb what it measures.
//
// ⚠️ IT RUNS ON ITS OWN HOST THREAD, ON A WALL CLOCK, ON PURPOSE. The previous
// version sampled from the guest's frame tick every 300 calls and printed
// exactly ONE line in a whole run — taken BEFORE sound init had run. I read
// that single pre-init sample as "the gate is 0 for the entire run" and built a
// whole (wrong) theory on it. A probe whose cadence is tied to the subsystem it
// is diagnosing can sample only the state it is trying to explain.
//
// What matters here is a RATE, not a level: each mix iteration submits one
// 6144-byte buffer = 256 frames = 5.333 ms of audio, so keeping up with a
// 48 kHz device takes 187.5 iterations/second. Anything less is a starving
// device, i.e. choppy sound, and the percentage says how far off we are.
// THE THROTTLE, read out of the engine rather than guessed. FUN_004725d0 is
// `return [0xe96e0c] < 6`, and the mix loop skips BOTH the mixer tick and the
// submit when it is false — so [0xe96e0c] is the engine's own count of buffers
// it believes are still queued on the device. `lock xadd [0xe96e0c], 1` at
// 0x631058 (right after SubmitSourceBuffer) raises it; `lock xadd [0xe96e0c],
// -1` at 0x647768 lowers it, and that instruction lives in a `ret 8` stdcall —
// i.e. IXAudio2VoiceCallback::OnBufferEnd(this, pBufferContext), the callback we
// deliver from the frame tick. So the mixer's rate is bounded by OUR callback
// delivery, and these two counters say whether the pump is even running.
static uint64_t g_pump_calls = 0, g_obe_delivered = 0;
extern uint64_t g_snd_ready_calls;      // FUN_004725d0 hits = mixer spin rate
// The Zombies exe runs the same Sound Mix loop at 0x449a60 (t6sp 0x572ca0: 71 instructions identical)
// on its own globals; the label printed is the address read.
#define SND_VA(sp, zm) sp
#define SND_STR_(x) #x
#define SND_STR(x) SND_STR_(x)
extern int      g_snd_tid;              // which guest thread the mixer runs on
extern "C" uint64_t    g_yield_count[32];
extern "C" const char* g_yield_last[32];
extern "C" uint32_t    g_yield_handle[32];
extern "C" uint64_t    g_pick_count[32];
extern "C" uint64_t    g_hold_ns[32];   // temps reel detenu par coroutine (scheduler)
extern "C" int         win32_thread_status(int tid);
extern "C" void bo2_audio_counters(uint64_t*, uint64_t*, uint64_t*, uint64_t*);
extern "C" float bo2_audio_peak();
extern "C" void  bo2_audio_peaks(float*);
extern uint64_t g_avail_calls, g_avail_nonzero;
extern uint64_t g_walk_calls;
extern uint64_t g_fir_calls, g_fir_bad;
extern uint64_t g_thunk_calls;
extern uint32_t g_read_by_file[64];
extern int32_t  g_fir_peak;

// BO2_MEM_SCAN=<hex va>:<hex len> — peak/activity of an arbitrary guest range,
// sampled by the audio sampler thread. The submitted PCM is digital silence, so
// the question is where the signal dies: point this at a buffer UPSTREAM of the
// mixer and see whether anything non-zero ever lands in it. Reported both as
// float (the engine mixes in 32-bit float) and as raw non-zero byte count, so it
// works whatever the buffer's format turns out to be.
static void mem_scan_report() {
    static const char* spec = bo2_getenv("BO2_MEM_SCAN");
    if (!spec) return;
    static uint32_t va = 0, len = 0;
    if (!va) {
        char* end = nullptr;
        va = (uint32_t)strtoul(spec, &end, 16);
        len = (end && *end == ':') ? (uint32_t)strtoul(end + 1, nullptr, 16) : 0x1000;
        if (len > 0x40000) len = 0x40000;
    }
    uint32_t nz = 0; float pk = 0.0f; int32_t ipk = 0;
    for (uint32_t o = 0; o + 4 <= len; o += 4) {
        uint32_t raw = win32_mem_read32(va + o);
        if (raw) nz++;
        float f; memcpy(&f, &raw, 4);
        if (f == f && f > -1e6f && f < 1e6f) { float a = f < 0 ? -f : f; if (a > pk) pk = a; }
        int16_t lo = (int16_t)(raw & 0xffff), hi = (int16_t)(raw >> 16);
        int32_t a1 = lo < 0 ? -lo : lo, a2 = hi < 0 ? -hi : hi;
        if (a1 > ipk) ipk = a1;
        if (a2 > ipk) ipk = a2;
    }
    // Aggregates answer "is there signal here?"; a raw dump answers "what is
    // this struct doing?". Both matter: the channel bookkeeping that gates the
    // whole audio chain is a handful of counters, and watching them move is the
    // only way to see a producer/consumer stall as it happens.
    char raw[16 * 11 + 1]; int ro = 0;
    for (uint32_t i = 0; i < 16 && i * 4 + 4 <= len; i++)
        ro += snprintf(raw + ro, sizeof raw - (size_t)ro, " %08x",
                       win32_mem_read32(va + i * 4));
    raw[ro] = 0;
    fprintf(stderr, "[MEM-SCAN 0x%08x+0x%x] nonzero dwords=%u/%u  peak(float)=%.5f "
                    "peak(int16)=%d |%s\n", va, len, nz, len / 4, pk, ipk, raw);
}

static void snd_sampler_start() {
    static bool started = false;
    if (started || !bo2_getenv("BO2_AUDIO_LOG")) return;
    started = true;
    std::thread([] {
        uint32_t last = win32_mem_read32(SND_VA(0xbf188c, 0xd6640c));
        auto     t0   = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            uint32_t it = win32_mem_read32(SND_VA(0xbf188c, 0xd6640c));
            uint64_t sub = 0, pl = 0, st = 0, dr = 0;
            bo2_audio_counters(&sub, &pl, &st, &dr);
            float pkch[8] = {}; bo2_audio_peaks(pkch);
            auto     t1 = std::chrono::steady_clock::now();
            double   dt = std::chrono::duration<double>(t1 - t0).count();
            double   r  = dt > 0 ? (it - last) / dt : 0.0;
            fprintf(stderr, "[SNDLOOP] gate[" SND_STR(SND_VA(0xbf1884, 0xd66404)) "]=%u iter[" SND_STR(SND_VA(0xbf188c, 0xd6640c)) "]=%u "
                            "(+%u in %.1fs = %.1f mix/s = %.0f%% of real time) "
                            "queued[" SND_STR(SND_VA(0xe96e0c, 0x100b98c)) "]=%d spin=%llu pump=%llu obe=%llu "
                            "| mix tid=%d yields=%llu last=%s(h=0x%x) st=%d picks=%llu "
                            "| dev sub=%llu play=%llu drop=%llu peak=%.4f avail=%llu/%llu walk=%llu fir=%llu/pk%d/bad%llu thunk=%llu ch=[%.4f %.4f %.4f %.4f %.4f %.4f]\n",
                    win32_mem_read32(SND_VA(0xbf1884, 0xd66404)), it, it - last, dt, r,
                    r * 100.0 / 187.5, (int32_t)win32_mem_read32(SND_VA(0xe96e0c, 0x100b98c)),
                    (unsigned long long)g_snd_ready_calls,
                    (unsigned long long)g_pump_calls,
                    (unsigned long long)g_obe_delivered,
                    g_snd_tid,
                    (unsigned long long)(g_snd_tid >= 0 && g_snd_tid < 32
                                         ? g_yield_count[g_snd_tid] : 0),
                    (g_snd_tid >= 0 && g_snd_tid < 32 && g_yield_last[g_snd_tid])
                        ? g_yield_last[g_snd_tid] : "-",
                    (g_snd_tid >= 0 && g_snd_tid < 32) ? g_yield_handle[g_snd_tid] : 0,
                    win32_thread_status(g_snd_tid),
                    (unsigned long long)(g_snd_tid >= 0 && g_snd_tid < 32
                                         ? g_pick_count[g_snd_tid] : 0),
                    (unsigned long long)(sub / 6144), (unsigned long long)(pl / 6144),
                    (unsigned long long)dr, bo2_audio_peak(),
                    (unsigned long long)g_avail_nonzero,
                    (unsigned long long)g_avail_calls,
                    (unsigned long long)g_walk_calls,
                    (unsigned long long)g_fir_calls, g_fir_peak,
                    (unsigned long long)g_fir_bad,
                    (unsigned long long)g_thunk_calls,
                    pkch[0], pkch[1], pkch[2], pkch[3], pkch[4], pkch[5]);
            {   // EVERY guest thread, not just the mixer. The menu goes black the
                // moment the sound subsystem is enabled, and the two candidate
                // explanations — "the render thread is blocked on something" and
                // "the render thread is runnable and the scheduler stopped picking
                // it" — need opposite fixes and look identical from outside. A
                // pick count DELTA over a wall-clock interval separates them in one
                // line: 0 picks + T_RUN is starvation, 0 picks + T_BLOCKED is a
                // wait, and a moving count means the thread is fine and the stall
                // is elsewhere.
                static uint64_t prev_pick[32] = {}, prev_hold[32] = {};
                // 📊 LE PARTAGE DU CŒUR. Le nombre d'elections ne dit pas le cout :
                // un thread elu 40 000 fois pour des tours de 2 us pese moins qu'un
                // thread elu 3 fois pour 150 ms. On imprime donc AUSSI la part du
                // temps reel que chacun a detenue sur l'intervalle. Les parts somment
                // a 100 % par construction (une seule coroutine tourne a la fois),
                // ce qui en fait un denominateur honnete — a la difference de la
                // moyenne des intervalles, interdite ici.
                uint64_t dhold[32] = {}, hold_tot = 0;
                for (int q = 0; q < 32; q++) {
                    dhold[q] = g_hold_ns[q] - prev_hold[q];
                    prev_hold[q] = g_hold_ns[q];
                    hold_tot += dhold[q];
                }
                char line[1500]; int o = 0;
                for (int q = 0; q < 32 && o < 1300; q++) {
                    int st = win32_thread_status(q);
                    if (st < 0) break;
                    uint64_t d = g_pick_count[q] - prev_pick[q];
                    prev_pick[q] = g_pick_count[q];
                    const char* why = (d == 0 && g_yield_last[q]) ? g_yield_last[q] : "";
                    const double pc = hold_tot ? dhold[q] * 100.0 / (double)hold_tot : 0.0;
                    o += snprintf(line + o, sizeof line - (size_t)o, " %d:%s%+lld/%.0f%%%s%s",
                                  q, st == 0 ? "RUN" : (st == 1 ? "BLK" : "END"),
                                  (long long)d, pc, *why ? "@" : "", why);
                }
                line[o] = 0;
                fprintf(stderr, "[SCHED] coeur=%.0fms%s\n", hold_tot / 1e6, line);
                // 🎯 Et la reculade des fils HOTES, qui ne se voit pas dans
                // `[SCHED]` : un fil sorti du tourniquet n'est plus elu, donc son
                // « +0/0% » ne dit pas s'il dort a bon escient ou s'il brule un
                // coeur. N'imprime rien quand aucune coroutine n'est hebergee.
                bo2_hostyield_report();
                bo2_filscpu_report();   // P795, toutes les ~10 s
            }
            {   // reads per zone slot, with the slot's path — the only honest
                // answer to "is this file ever read?".
                char line[1600]; int o = 0; int shown = 0;
                for (int i = 0; i < 64 && shown < 40; i++) {
                    if (!g_read_by_file[i]) continue;
                    // ⚠️ WIDE ENOUGH TO SEE THE EXTENSION. At 24 characters the base
                    // name truncates to 15 and "cmn_root.all.sabs" is
                    // indistinguishable from "cmn_root.all.sabl" — which is exactly
                    // the distinction that matters, .sabs being a STREAMED bank and
                    // .sabl a LOADED one. Slots [20]=2 and [21]=68 look like that
                    // very pair and the log could not say which was which.
                    char nm[68]; int k = 0;
                    const uint32_t slot = 0x291f198u + (uint32_t)i * 0x120u;
                    while (k < 64) { uint8_t c = win32_mem_read8(slot + (uint32_t)k);
                                     if (c < 0x20 || c > 0x7e) break; nm[k++] = (char)c; }
                    nm[k] = 0;
                    const char* base = strrchr(nm, 92);   // last backslash
                    o += snprintf(line + o, sizeof line - (size_t)o, " [%d]%s=%u",
                                  i, base ? base + 1 : (k ? nm : "?"), g_read_by_file[i]);
                    shown++;
                }
                line[o] = 0;
                fprintf(stderr, "[READS]%s\n", o ? line : " (none)");
            }
            mem_scan_report();
            last = it; t0 = t1;
        }
    }).detach();
}

extern "C" int g_in_shim_callback;
extern thread_local int t_in_shim_cb;      // P642/C, kernel32_impl.cpp
extern "C" void bo2_xaudio_pump(State* s) {
    snd_sampler_start();
    g_pump_calls++;
    if (!s || !g_xa_voice_cb || !g_xa_unit) return;
    // OnBufferEnd is guest code and it ends in SetEvent. Guard both hazards here
    // rather than at each call site: re-entering on the same State would corrupt
    // it, and letting that SetEvent yield costs a whole scheduler rotation per
    // finished buffer.
    // 🔴 P642/D — EXCLUSIF ENTRE FILS, plus seulement par fil. OnBufferEnd
    // est appele par le fil 0 a chaque image ET par le fil du son : heberge,
    // `thread_local` laissait les deux fils livrer en meme temps les memes
    // tampons (take_completed, compteurs, etat XAudio invite). Le perdant rend
    // la main, exactement comme la reentree sur un meme fil le faisait deja.
    static std::atomic<bool> busy{false};
    if (busy.exchange(true, std::memory_order_acquire)) return;
    struct Guard {
        Guard()  { g_in_shim_callback++; t_in_shim_cb++; }
        ~Guard() { t_in_shim_cb--; g_in_shim_callback--; busy.store(false, std::memory_order_release); }
    } guard;
    // Resolve the callback BEFORE asking for credit: take_completed consumes
    // what it hands back, so asking without being able to deliver loses those
    // completions permanently — and losing six is enough to livelock the engine
    // at its 6-buffer ceiling for the rest of the run.
    uint32_t vt = win32_mem_read32(g_xa_voice_cb);
    uint32_t fn = vt ? win32_mem_read32(vt + 4 * 4) : 0;     // slot 4 = OnBufferEnd
    if (!fn) return;
    // Through the journal: the device's consumption rate is host timing, and it
    // decides how many times guest code runs below. See bo2_replay.cpp kAudioSlot.
    extern uint32_t bo2_journal_audio_take(uint32_t, uint32_t);
    uint32_t done = bo2_journal_audio_take(g_xa_unit, 4096);
    if (!done) return;
    // `done` is now exactly what we asked for and are committed to delivering.
    for (uint32_t i = 0; i < done; i++) {
        uint32_t save_esp = s->gpr.rsp.dword, save_eax = s->gpr.rax.dword;
        uint32_t sp = save_esp;
        sp -= 4; win32_mem_write32(sp, 0);                   // pBufferContext
        sp -= 4; win32_mem_write32(sp, g_xa_voice_cb);       // this
        sp -= 4; win32_mem_write32(sp, 0xDEAD0000u);         // return sentinel
        s->gpr.rsp.dword = sp;
        win32_run_lifted(s, fn);
        s->gpr.rsp.dword = save_esp; s->gpr.rax.dword = save_eax;
        g_obe_delivered++;
    }
}
extern "C" bool xaudio_vtbl_dispatch(State& s, uint32_t addr) {
    if ((addr & 0xFF000000u) != DS_VTBL_BASE) return false;
    int iface = (int)((addr >> 8) & 0xFFu), slot = (int)(addr & 0xFFu);
    if (iface < XA_IF_ENGINE) return false;          // DSOUND ifaces keep their own path
    // COUNT, do not just mark seen. A first-use-only log showed each method once
    // and so made TWO complete sound-init passes look like one — which is exactly
    // what hid the second pass zeroing the voice pointer.
    static uint32_t hits[16][DS_VTBL_SLOTS] = {};
    if (iface < 16 && slot < DS_VTBL_SLOTS) {
        uint32_t& c = hits[iface][slot];
        if (++c == 1 || c == 2 || (c % 500) == 0)
            fprintf(stderr, "[XAUDIO] iface %d slot %d call #%u (ret=0x%08x) "
                            "voice[" SND_STR(SND_VA(0xe96e00, 0x100b980)) "]=0x%08x\n",
                    iface, slot, c, win32_mem_read32(s.gpr.rsp.dword),
                    win32_mem_read32(SND_VA(0xe96e00, 0x100b980)));
    }
    uint32_t esp = s.gpr.rsp.dword;
    auto a = [&](int i) { return win32_mem_read32(esp + 4 * i); };   // a(1)=this
    // ⚠️ IXAudio2Voice IS NOT IUnknown-DERIVED. Unlike every other COM interface
    // in this shim, a voice has no QueryInterface/AddRef/Release: slot 0 is
    // GetVoiceDetails. Treating 0/1/2 as IUnknown on a voice would answer the
    // wrong method AND pop the wrong number of arguments.
    const bool is_voice = (iface >= XA_IF_MASTER);
    // --- OnBufferEnd, delivered on the GUEST thread ---------------------------
    // XAudio2 reports a finished buffer through IXAudio2VoiceCallback::OnBufferEnd,
    // and BO2's mixer will not submit more until it hears that: it queued 6
    // buffers (32 ms) and then waited forever. We CANNOT call it from the
    // AudioQueue thread — that is a host thread with no guest State, and entering
    // lifted code there would corrupt whichever guest thread's registers we
    // borrowed. But every voice method arrives here ON a guest thread, so this is
    // a safe place to flush the completions the device has actually finished.
    // Vtable: 0 OnVoiceProcessingPassStart, 1 ...End, 2 OnStreamEnd,
    //         3 OnBufferStart, 4 OnBufferEnd, 5 OnLoopEnd, 6 OnVoiceError.
    if (is_voice) bo2_xaudio_pump(&s);
    int nargs;
    if (is_voice) nargs = kXAVoiceArgs[slot < 29 ? slot : 0];
    else          nargs = (slot < 16) ? kXA2Args[slot] : 1;
    if (!is_voice) {
        if (slot == 1 || slot == 2) { s.gpr.rax.dword = 1; win32_stdcall_ret(s, 1); return true; }
        if (slot == 0) { uint32_t ppv = a(3); if (ppv) win32_mem_write32(ppv, ds_iface_obj(iface));
                         s.gpr.rax.dword = 0; win32_stdcall_ret(s, 3); return true; }
    } else if (slot == 0) {                       // GetVoiceDetails(this, *details)
        uint32_t p = a(2);                        // {CreationFlags, ActiveFlags, InputChannels, InputSampleRate}
        if (p) { win32_mem_write32(p + 0, 0); win32_mem_write32(p + 4, 0);
                 win32_mem_write32(p + 8, 2); win32_mem_write32(p + 12, 48000); }
        s.gpr.rax.dword = 0; win32_stdcall_ret(s, 2); return true;
    } else if (slot == 21) {                      // SubmitSourceBuffer(this,pBuffer,pWMA)
        // XAUDIO2_BUFFER: Flags +0, AudioBytes +4, pAudioData +8, PlayBegin +12,
        // PlayLength +16, LoopBegin +20, LoopLength +24, LoopCount +28,
        // pContext +32. This is where the engine's mixed PCM finally leaves the
        // guest, so it is the one place worth copying out.
        uint32_t b = a(2);
        if (b) {
            uint32_t bytes = win32_mem_read32(b + 4), data = win32_mem_read32(b + 8);
            if (data && bytes && bytes <= (4u << 20)) {
                g_xa_unit = bytes;
                static thread_local std::vector<uint8_t> tmp;   // P661/C15 : un tampon par fil hote
                tmp.resize(bytes);
                win32_mem_read_block(tmp.data(), data, bytes);
                bo2_audio_submit(tmp.data(), bytes);
            }
        }
        s.gpr.rax.dword = 0; win32_stdcall_ret(s, nargs); return true;
    } else if (slot == 25) {                      // GetState(this, *pVoiceState)
        // XAUDIO2_VOICE_STATE { void* pCurrentBufferContext; UINT32 BuffersQueued;
        //                       UINT64 SamplesPlayed; }  (SamplesPlayed is 8-aligned)
        uint32_t p = a(2);
        if (p) {
            uint32_t queued = 0; uint64_t played = 0;
            bo2_audio_state(&queued, &played);
            win32_mem_write32(p + 0,  0);                       // pCurrentBufferContext
            win32_mem_write32(p + 4,  queued);
            win32_mem_write32(p + 8,  (uint32_t)(played & 0xffffffffu));
            win32_mem_write32(p + 12, (uint32_t)(played >> 32));
        }
        s.gpr.rax.dword = 0; win32_stdcall_ret(s, nargs); return true;
    }
    if (iface == XA_IF_ENGINE) {
        switch (slot) {
            case 3: if (a(2)) win32_mem_write32(a(2), 1); break;      // one device
            case 4: {                                                  // GetDeviceDetails
                // XAUDIO2_DEVICE_DETAILS: WCHAR DeviceID[256] at 0, WCHAR
                // DisplayName[256] at 0x200, XAUDIO2_DEVICE_ROLE at 0x400,
                // WAVEFORMATEXTENSIBLE OutputFormat at 0x404.
                // THE FIELDS ARE 16-BIT AND THAT MATTERS: the engine's device
                // filter FUN_00885ba0 reads `word [details + 0x406]`, i.e.
                // nChannels, and rejects anything below 2. Writing this struct
                // in dwords put 0 there and every device was refused.
                uint32_t p = a(3);
                if (p) {
                    for (int i = 0; i < 0x460; i += 4) win32_mem_write32(p + i, 0);
                    static const char kName[] = "CoreAudio";
                    for (int i = 0; kName[i]; i++) {                   // UTF-16LE
                        win32_mem_write16(p + 0x000 + i * 2, (uint16_t)kName[i]);
                        win32_mem_write16(p + 0x200 + i * 2, (uint16_t)kName[i]);
                    }
                    win32_mem_write32(p + 0x400, 0x3f);                // Role: all roles
                    uint32_t f = p + 0x404;                            // WAVEFORMATEXTENSIBLE
                    win32_mem_write16(f + 0,  0xFFFE);                 // WAVE_FORMAT_EXTENSIBLE
                    win32_mem_write16(f + 2,  2);                      // nChannels
                    win32_mem_write32(f + 4,  48000);                  // nSamplesPerSec
                    win32_mem_write32(f + 8,  48000 * 2 * 2);          // nAvgBytesPerSec
                    win32_mem_write16(f + 12, 4);                      // nBlockAlign
                    win32_mem_write16(f + 14, 16);                     // wBitsPerSample
                    win32_mem_write16(f + 16, 22);                     // cbSize
                    win32_mem_write16(f + 18, 16);                     // wValidBitsPerSample
                    win32_mem_write32(f + 20, 3);                      // FL | FR
                    // SubFormat = KSDATAFORMAT_SUBTYPE_PCM
                    win32_mem_write32(f + 24, 0x00000001);
                    win32_mem_write32(f + 28, 0x00100000);
                    win32_mem_write32(f + 32, 0xAA000080);
                    win32_mem_write32(f + 36, 0x719B3800);
                }
                break; }
            case 8: case 9: case 10: {                                 // voice factories
                uint32_t pp = a(2);   // ppVoice is always the first real arg
                int vif = slot == 10 ? XA_IF_MASTER : (slot == 8 ? XA_IF_SOURCE : XA_IF_SUBMIX);
                if (pp) { uint32_t vo = ds_iface_new(vif);
                          win32_mem_write32(pp, vo);
                          if (bo2_getenv("BO2_AUDIO_LOG"))
                              fprintf(stderr, "[XAUDIO] voice %08x created (slot %d) at %08x\n",
                                      vo, slot, pp); }
                if (slot == 8) {                       // CreateSourceVoice
                    g_xa_voice_cb = a(6);              // pCallback (IXAudio2VoiceCallback*)
                    uint32_t wf = a(3);                // WAVEFORMATEX*
                    if (wf) bo2_audio_set_format((int)(win32_mem_read32(wf + 0) >> 16),
                                                 (int)win32_mem_read32(wf + 4),
                                                 (int)(win32_mem_read32(wf + 12) >> 16));
                }
                break; }
            default: break;
        }
    }
    s.gpr.rax.dword = 0;                                               // S_OK
    win32_stdcall_ret(s, nargs);
    return true;
}
static bool xaudio_enabled() {
    // ⚠️ "0" MUST MEAN OFF. This was `getenv(...) ? 1 : 0`, so BO2_XAUDIO=0 read
    // as ON and there was no way to run the no-sound path from a script that
    // sets the variable (play.sh does, unconditionally). A flag that cannot be
    // turned off is not a flag, and it hid an A/B this port needed.
    const char* v = bo2_getenv("BO2_XAUDIO");
    static const int e = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    return e != 0;
}
static void impl_CoCreateInstance(State& s) {
    uint32_t ppv = win32_arg(s, 4);
    // The fake engine answers for the XAudio2 2.7 class and its debug twin, nothing else. The Zombies exe also
    // builds its webcam's DirectShow graph at startup (FilterGraph, CaptureGraphBuilder2, SystemDeviceEnum,
    // SampleGrabber): handed an IXAudio2, it called null vtable slots (run 233302). t6sp only asks for XAudio2.
    static const uint8_t kXAudio2[2][16] = {
        {0x85,0x86,0x50,0x5a,0x54,0xa2,0xba,0x4f,0x9b,0x82,0x9a,0x24,0xb0,0x03,0x06,0xaf},   // 5A508685-…
        {0x35,0xea,0x05,0xdb,0x29,0x03,0x4b,0x4d,0xa5,0x3a,0x6d,0xea,0xd0,0x3d,0x38,0x52}};  // DB05EA35-… (debug)
    uint8_t clsid[16];
    for (uint32_t i = 0; i < 16; i++) clsid[i] = win32_mem_read8(win32_arg(s, 0) + i);
    const bool is_xaudio2 = !memcmp(clsid, kXAudio2[0], 16) || !memcmp(clsid, kXAudio2[1], 16);
    if (!is_xaudio2)
        fprintf(stderr, "[COM] CoCreateInstance {%02x%02x%02x%02x-%02x%02x-%02x%02x-…} (ret=0x%08x) -> REGDB_E_CLASSNOTREG\n",
                clsid[3], clsid[2], clsid[1], clsid[0], clsid[5], clsid[4], clsid[7], clsid[6],
                win32_mem_read32(s.gpr.rsp.dword));
    if (is_xaudio2 && xaudio_enabled()) {
        if (ppv) win32_mem_write32(ppv, ds_iface_obj(XA_IF_ENGINE));
        s.gpr.rax.dword = 0;             // S_OK
        fprintf(stderr, "[XAUDIO] CoCreateInstance -> fake IXAudio2\n");
        win32_stdcall_ret(s, 5);
        return;
    }
    if (ppv) win32_mem_write32(ppv, 0);
    s.gpr.rax.dword = 0x80040154u;   // REGDB_E_CLASSNOTREG
    win32_stdcall_ret(s, 5);
}
DLL_STUB0(CoUninitialize)
DLL_STUB2(CLSIDFromString)
DLL_STUB2(CoInitializeEx)
DLL_STUB1(CoInitialize)             // (pvReserved) -> S_OK ; importe par t6zm seulement (Z-NATIF-08d)

// --- XINPUT1_3 (3 functions, by ordinal #2,3,4) ---
// XInputGetState, XInputSetState, XInputGetCapabilities
// Return ERROR_DEVICE_NOT_CONNECTED (0x48F)
// XInputGetState. 0x48F = ERROR_DEVICE_NOT_CONNECTED, which is the honest
// answer until a GameController is wired up — the engine then treats pad 0 as
// absent and falls back to keyboard/mouse.
// BO2_FAKE_PAD=1 — report ONE connected, idle controller on index 0.
//
// Why this is worth trying, and it is a hypothesis with a mechanism behind it:
// the lockout screen's OpenMainMenu path (ui/t6/menus/lockoutmenu.lua, its own
// string constants) begins
//     Engine.ExecNow("remapcontrollertoprimaryandactivate")
//     UIExpression.GetPrimaryController
//     ... anyControllerAllowed / useController / PLATFORM_PRESS_START_BUTTON
// and UIExpression.IsControllerBeingUsed is evaluated TWELVE times after the
// keypress and never before it. On a console-derived title, PRESS START means
// "bind the controller that pressed it as primary" — and this shim tells the
// game, for every index, that no device is connected. Return dismisses the
// screen (keyboard input works) but the controller half of the handshake has
// nothing to bind.
//
// XINPUT_STATE = { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } where
// XINPUT_GAMEPAD = { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
//                    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } = 12 bytes,
// so the struct is 16 bytes. An idle pad is all zeros; the packet number must
// change only when the state changes, so it stays constant here.
// La manette elle-meme vit dans runtime/input/pad_gc.mm (GameController ou BO2_PAD_SCRIPT) ;
// ici on ne fait que copier ses octets au format Windows dans la memoire invitee.
extern "C" uint32_t bo2_pad_etat(uint32_t idx, uint8_t out[16]);
extern "C" uint32_t bo2_pad_capacites(uint32_t idx, uint8_t out[20]);
extern "C" uint32_t bo2_pad_vibrer(uint32_t idx, uint16_t gauche, uint16_t droite);
static void pad_ecrire(uint32_t va, const uint8_t* src, uint32_t n) {
    for (uint32_t o = 0; o < n; o += 4) {
        uint32_t v; std::memcpy(&v, src + o, 4); win32_mem_write32(va + o, v);
    }
}
static void impl_xinput_ordinal_2(State& s) {   // XInputGetState(dwUserIndex, pState)
    g_n_pad_poll++;
    const uint32_t idx    = win32_mem_read32(s.gpr.rsp.dword + 4);
    const uint32_t pState = win32_mem_read32(s.gpr.rsp.dword + 8);
    uint8_t st[16];
    const uint32_t rc = bo2_pad_etat(idx, st);
    if (rc == 0 && pState) pad_ecrire(pState, st, 16);
    s.gpr.rax.dword = rc; win32_stdcall_ret(s, 2);
}
static void impl_xinput_ordinal_3(State& s) {   // XInputSetState(dwUserIndex, pVibration)
    const uint32_t idx  = win32_mem_read32(s.gpr.rsp.dword + 4);
    const uint32_t pVib = win32_mem_read32(s.gpr.rsp.dword + 8);
    const uint32_t v = pVib ? win32_mem_read32(pVib) : 0;   // { WORD gauche, WORD droite }
    s.gpr.rax.dword = bo2_pad_vibrer(idx, (uint16_t)(v & 0xffff), (uint16_t)(v >> 16));
    win32_stdcall_ret(s, 2);
}
static void impl_xinput_ordinal_4(State& s) {   // XInputGetCapabilities(dwUserIndex, dwFlags, pCaps)
    const uint32_t idx  = win32_mem_read32(s.gpr.rsp.dword + 4);
    const uint32_t pCap = win32_mem_read32(s.gpr.rsp.dword + 0xc);
    uint8_t cap[20];
    const uint32_t rc = bo2_pad_capacites(idx, cap);
    if (rc == 0 && pCap) pad_ecrire(pCap, cap, 20);
    s.gpr.rax.dword = rc; win32_stdcall_ret(s, 3);
}

// --- DSOUND (2 exports, by ordinal #6,#11) ----------------------------------
// DirectSoundCreate(LPCGUID, LPDIRECTSOUND*, IUnknown*) — 3 stdcall args.
// DirectSoundCaptureCreate(LPCGUID, LPDIRECTSOUNDCAPTURE*, IUnknown*) — 3 args.
//
// These used to return DSERR_NODRIVER on purpose, so the engine took its
// "no sound card" path. That was the right call while nothing could play audio,
// but it costs more than silence: FUN_0051cdd0 (the mixer tick) only runs the
// rest of the chain when [0xe87384] is non-zero, and one of the things it drives
// is FUN_008857c0 -> the VIDEO subsystem's audio callback. So declining
// DirectSound also stops half the WebM path from ever pumping.
//
// BO2_DSOUND=1 hands back a real fake IDirectSound8 instead, on the same
// sentinel-vtable machinery as D3D11 and Steam (0xE5iissss here). The point of
// the first increment is DISCOVERY, exactly as it was for D3D11: accept the
// interface, log which vtable slots the engine actually calls, and let that list
// decide what has to be implemented — rather than guessing at the whole of
// IDirectSound8 / IDirectSoundBuffer up front. Off by default: a half-built
// audio device that the engine believes in is worse than an honest refusal.
static bool ds_enabled() {
    static const int e = bo2_getenv("BO2_DSOUND") ? 1 : 0;
    return e != 0;
}
// Dispatch for the 0xE5iissss sentinels. Every slot is logged on FIRST USE, so
// one run produces the exact method list to implement. Unknown slots return
// DS_OK with a zeroed out-param, which keeps the engine walking forward instead
// of erroring out on the first unimplemented call.
extern "C" bool dsound_vtbl_dispatch(State& s, uint32_t addr) {
    if ((addr & 0xFF000000u) != DS_VTBL_BASE) return false;
    int iface = (int)((addr >> 8) & 0xFFu);
    int slot  = (int)(addr & 0xFFu);
    static bool seen[8][DS_VTBL_SLOTS] = {};
    if (iface < 8 && slot < DS_VTBL_SLOTS && !seen[iface][slot]) {
        seen[iface][slot] = true;
        fprintf(stderr, "[DSOUND] iface %d slot %d first use (ret=0x%08x)\n",
                iface, slot, win32_mem_read32(s.gpr.rsp.dword));
    }
    // IUnknown is the same in every COM interface: 0 QueryInterface, 1 AddRef,
    // 2 Release. Getting those wrong makes the engine free an object it still
    // uses, which surfaces far away from here.
    if (slot == 1 || slot == 2) { s.gpr.rax.dword = 1; win32_stdcall_ret(s, 1); return true; }
    if (slot == 0) {                                   // QueryInterface(this,riid,ppv)
        uint32_t ppv = win32_mem_read32(s.gpr.rsp.dword + 12);
        if (ppv) win32_mem_write32(ppv, ds_iface_obj(iface));
        s.gpr.rax.dword = 0; win32_stdcall_ret(s, 3); return true;
    }
    // Everything else: succeed, and zero a plausible out-param so a pointer the
    // engine reads back is NULL rather than stack garbage.
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 1);
    return true;
}
static void impl_dsound_ordinal_6(State& s) {
    uint32_t ppDS = win32_arg(s, 1);
    if (ds_enabled()) {
        if (ppDS) win32_mem_write32(ppDS, ds_iface_obj(DS_IF_DEVICE));
        s.gpr.rax.dword = 0;                   // DS_OK
        fprintf(stderr, "[DSOUND] DirectSoundCreate -> fake IDirectSound8\n");
        win32_stdcall_ret(s, 3);
        return;
    }
    if (ppDS) win32_mem_write32(ppDS, 0);  // *ppDS = NULL
    s.gpr.rax.dword = 0x88780078;          // DSERR_NODRIVER
    win32_stdcall_ret(s, 3);
}
static void impl_dsound_ordinal_11(State& s) {
    uint32_t ppDSC = win32_arg(s, 1);
    if (ppDSC) win32_mem_write32(ppDSC, 0);
    s.gpr.rax.dword = 0x88780078;          // DSERR_NODRIVER
    win32_stdcall_ret(s, 3);
}

// --- SHELL32 (2 functions) ---
#include <unistd.h>
#include <spawn.h>
#include <fcntl.h>
extern char** environ;
// ShellExecuteA(hwnd, verbe, fichier, parametres, repertoire, affichage). Le jeu s'en sert pour passer d'un executable
// a l'autre : entrees CAMPAIGN / ZOMBIES / MULTIPLAYER des menus -> "steam://run/<appid>" (202970 campagne, 212910
// Zombies, 202990 multijoueur). Sous Windows, Steam lance alors l'autre jeu. Ici un relais attend la fin de ce processus
// (un seul `boot` a la fois) puis lance `scripts/launch.sh <mode>`, le lanceur commun aux trois. Temoin [SHELLEXEC].
// Relais commun : `fichier` nomme le jeu vise ("steam://run/<appid>"). Rend vrai si un lanceur a ete programme.
extern "C" bool bo2_relais_jeu(const char* fichier) {
    const char* mode = strstr(fichier, "steam://run/202970") ? "campagne"
                     : strstr(fichier, "steam://run/212910") ? "zombies"
                     : strstr(fichier, "steam://run/202990") ? "multi" : nullptr;
    fprintf(stderr, "[SHELLEXEC] \"%.200s\" -> %s\n", fichier, mode ? mode : "non servi");
    if (!mode) return false;
    // Environnement vide : les BO2_* de ce jeu (tubes, fils heberges, leviers des Zombies) ne valent pas pour l'autre. Passent
    // seulement le choix du panneau, le nom du joueur et le HUD Metal (scripts/launch.sh).
    std::string garde;
    for (const char* v : {"BO2_PANNEAU", "BO2_NAME", "MTL_HUD_ENABLED", "BO2_TITRE"})
        if (const char* x = getenv(v)) garde += std::string(v) + "='" + x + "' ";
    const char* lanceur = "scripts/play_steam.sh";   // le build Steam reste sur les executables Steam
    mode = mode[0] == 'c' ? "sp" : mode[0] == 'z' ? "zm" : "mp";
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "(while kill -0 %d 2>/dev/null; do sleep 1; done; pkill -f scripts/panel.py; "
             "exec env -i HOME=\"$HOME\" PATH=\"$PATH\" USER=\"$USER\" TMPDIR=\"$TMPDIR\" %s %s %s) "
             ">/dev/null 2>&1 &", (int)getpid(), garde.c_str(), lanceur, mode);
    // posix_spawn sans attente, pas system() : lance depuis l'app, system() a rendu une erreur sans rien ouvrir (10-22).
    pid_t pid;
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", cmd, nullptr};
    // CLOEXEC_DEFAULT : le relais n'herite que de 0/1/2 (vers /dev/null). Il heritait des milliers de descripteurs du
    // jeu : « cannot make pipe for command substitution: Too many open files » dans launch.sh, cd vers /, rien ne
    // s'ouvrait (24/10) ; un autre essai, dyld abandonne dans CacheFinder (cache partage non ouvrable).
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);   // hors de la session du jeu
    const int rc = posix_spawn(&pid, "/bin/sh", &fa, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) fprintf(stderr, "[SHELLEXEC] relais non lance : %s\n", strerror(rc));
    return true;
}
static void impl_ShellExecuteA(State& s) {
    const uint32_t f = win32_arg(s, 2);
    const char* url = f ? (const char*)(bo2_g_base + f) : "";
    s.gpr.rax.dword = bo2_relais_jeu(url) ? 42u : 31u;   // > 32 = succes ; 31 = SE_ERR_NOASSOC
    win32_stdcall_ret(s, 6);
}
DLL_STUB5(SHGetFolderPathA)

// --- WTSAPI32 (2 functions) ---
DLL_STUB1(WTSUnRegisterSessionNotification)
DLL_STUB2(WTSRegisterSessionNotification)

// --- D3D11 / DXGI fake-COM emulation (renderer bring-up) ---
// The renderer (FUN_006bb730) creates a DXGI factory + D3D11 device via
// CreateDXGIFactory1 / D3D11CreateDevice, then drives them through COM vtables:
// CreateDXGIFactory1(riid,&f) -> f->EnumAdapters1(0,&a) (slot 12) -> ... ->
// D3D11CreateDevice(...,&dev,&fl,&ctx) -> swapchain -> draw. Return fake objects
// whose 256-slot vtables are sentinels D3D_VTBL_BASE|(iface<<8)|slot; an indirect
// `call [vtable+slot*4]` routes through __remill_function_call to
// d3d_vtbl_dispatch(), which answers the methods the renderer checks (measure ->
// implement, exactly like the Steam vtable emulation above). COM on x86 =
// __stdcall with `this` as the FIRST STACK arg (callee cleans); HRESULT S_OK=0,
// negative = failure (the game checks the sign bit with `js`).
enum { D3D_IF_FACTORY = 1, D3D_IF_ADAPTER = 2, D3D_IF_OUTPUT = 3,
       D3D_IF_DEVICE = 4, D3D_IF_CONTEXT = 5, D3D_IF_SWAPCHAIN = 6,
       D3D_IF_RESOURCE = 7, D3D_IF_VIEW = 8, D3D_IF_DXGIDEV = 9 };
static constexpr uint32_t D3D_VTBL_BASE  = 0xE6000000u;
static constexpr int      D3D_VTBL_SLOTS = 256;

// ── 🎯 P651 — LE VERROU DU PERIPHERIQUE D3D11 ──────────────────────────────
// Sous Windows, ID3D11Device est libre-fil : le pilote serialise lui-meme les
// creations de ressources appelees depuis n'importe quel fil. Chez nous, tout
// l'etat hote de l'emulation (liaisons du contexte, registres de textures, de
// tampons, de shaders, textures dynamiques) est en statiques nus, et n'a tenu
// que parce que toutes les coroutines partageaient UN fil hote. Heberger le
// fil de rendu (BO2_HOST_THREADS=…,backend) le fait appeler en vrai parallele
// avec Main, Database et le lecteur de flux.
// Audit (lecture seule, 2026-09-25) : cet etat n'est touche QUE par
// d3d_vtbl_dispatch et les trois imports de creation ; rien n'y cede, n'y
// attend ni ne rappelle du code invite, et les gfx_* appeles ne prennent que
// des mutex feuilles. Un seul mutex, pris apres le test de sentinelle, suffit.
// Temoins : [D3D-TID] (premier couple interface/methode/tid vu) et
// [D3D-MU #N] (acquisition contendue, en puissances de deux, attente en us).
static std::mutex g_d3d_mu;
static uint64_t   g_d3d_contended = 0;       // sous g_d3d_mu
static uint64_t   g_d3d_wait_us   = 0;       // sous g_d3d_mu
static uint32_t   g_d3d_tids[16][D3D_VTBL_SLOTS];  // tids vus par methode, sous g_d3d_mu

static std::unique_lock<std::mutex> d3d_lock(int iface, int slot) {
    std::unique_lock<std::mutex> lk(g_d3d_mu, std::try_to_lock);
    if (!lk.owns_lock()) {
        const auto t0 = std::chrono::steady_clock::now();
        lk.lock();
        const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        g_d3d_wait_us += us;
        const uint64_t k = ++g_d3d_contended;
        if ((k & (k - 1)) == 0)
            fprintf(stderr, "[D3D-MU #%llu] tid=%d if=%d slot=%d attente=%llu us "
                            "cumul=%llu us\n", (unsigned long long)k, win32_current_tid(),
                    iface, slot, (unsigned long long)us, (unsigned long long)g_d3d_wait_us);
    }
    return lk;
}

// One fake object per interface id (singletons; the renderer keeps a single
// device/factory). obj layout: [+0]=vtable, [+4..]=scratch fields.
static uint32_t d3d_iface_obj(int id) {
    static uint32_t objs[16] = {0};
    if (id < 1 || id >= 16) return 0;
    if (objs[id]) return objs[id];
    uint32_t vtbl = win32_guest_alloc(D3D_VTBL_SLOTS * 4, 4);
    for (int i = 0; i < D3D_VTBL_SLOTS; i++)
        win32_mem_write32(vtbl + i * 4, D3D_VTBL_BASE | (uint32_t)(id << 8) | (uint32_t)i);
    uint32_t obj = win32_shim_alloc(64, 4);
    win32_mem_write32(obj + 0, vtbl);
    for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
    objs[id] = obj;
    return obj;
}

// CreateDXGIFactory1(REFIID riid, void** ppFactory) — stdcall 2. S_OK + factory.
static void impl_CreateDXGIFactory1(State& s) {
    const auto lk = d3d_lock(0, 0);          // d3d_iface_obj partage objs[] avec le repartiteur
    uint32_t ppFactory = win32_arg(s, 1);
    if (ppFactory) win32_mem_write32(ppFactory, d3d_iface_obj(D3D_IF_FACTORY));
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 2);
}

// D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels,
//   FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext)
// — stdcall 10. Write the out-params and return S_OK.
static void impl_D3D11CreateDevice(State& s) {
    const auto lk = d3d_lock(0, 0);
    uint32_t ppDevice  = win32_arg(s, 7);
    uint32_t pFL       = win32_arg(s, 8);
    uint32_t ppContext = win32_arg(s, 9);
    if (ppDevice)  win32_mem_write32(ppDevice,  d3d_iface_obj(D3D_IF_DEVICE));
    if (pFL)       win32_mem_write32(pFL, 0xB000u);          // D3D_FEATURE_LEVEL_11_0
    if (ppContext) win32_mem_write32(ppContext, d3d_iface_obj(D3D_IF_CONTEXT));
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 10);
}

// D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
//   pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
//   ppDevice, pFeatureLevel, ppImmediateContext) — stdcall 12.
static void impl_D3D11CreateDeviceAndSwapChain(State& s) {
    const auto lk = d3d_lock(0, 0);
    uint32_t ppSwap    = win32_arg(s, 8);
    uint32_t ppDevice  = win32_arg(s, 9);
    uint32_t pFL       = win32_arg(s, 10);
    uint32_t ppContext = win32_arg(s, 11);
    if (ppSwap)    win32_mem_write32(ppSwap,    d3d_iface_obj(D3D_IF_SWAPCHAIN));
    if (ppDevice)  win32_mem_write32(ppDevice,  d3d_iface_obj(D3D_IF_DEVICE));
    if (pFL)       win32_mem_write32(pFL, 0xB000u);
    if (ppContext) win32_mem_write32(ppContext, d3d_iface_obj(D3D_IF_CONTEXT));
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 12);
}

// Decode a D3D/DXGI vtable-method sentinel. Returns false if `addr` is not a
// D3D sentinel (caller continues normal dispatch). Every COM object shares
// IUnknown at slots 0/1/2 (QueryInterface/AddRef/Release).
// Traduit un shader DXBC -> MSL et le met en file pour Metal (slot 9 = PS, 11 = VS). Appele a la premiere liaison,
// ou des sa creation sous BO2_SHADER_CREE=1 : sous Windows le pilote compile a CreatePixelShader/CreateVertexShader,
// pendant le chargement de la carte ; a la premiere liaison, chaque nouveau shader fige une image en pleine partie.
static void translate_shader(uint32_t so, int slot) {
    if (!g_window_gfx || g_sh_done[so]) return;
    if (!gfx_shaders_open()) {                    // fenetre pas encore ouverte : traduit a la premiere liaison
        static uint32_t nrep = 0;
        if (++nrep <= 3) fprintf(stderr, "[MSL-REPORT] %u shader(s) cree(s) avant la fenetre : traduits a la liaison\n", nrep);
        return;
    }
    auto it = g_sh_bc.find(so);
    if (it == g_sh_bc.end() || it->second.empty()) return;
    g_sh_done[so] = 1;
    char* msl = nullptr; char* tmap = nullptr; char* cbmap = nullptr;
    char* tdim = nullptr; char* smap = nullptr;
    static uint32_t nok = 0, nbad = 0;
    if (bo2_dxbc_translate2(it->second.data(), (uint32_t)it->second.size(),
                            &msl, &tmap, &cbmap, &tdim, &smap) && msl) {
        // READ THE TRANSLATED SHADER before theorising about what it
        // wants — that reflex has already saved four build-and-look
        // cycles on this project. BO2_MSL_DUMP=1 writes every one of
        // them to msl_dump/ so its texture registers, constant buffers
        // and inputs can be checked against what we actually bind.
        static const int msl_dump = bo2_getenv("BO2_MSL_DUMP") ? 1 : 0;
        if (msl_dump) {
            mkdir("msl_dump", 0755);
            char p[64];
            snprintf(p, sizeof p, "msl_dump/%08x_%s.msl", so,
                     slot == 9 ? "ps" : "vs");
            FILE* mf = fopen(p, "w");
            if (mf) { fputs(msl, mf); fclose(mf); }
        }
        if (strstr(msl, "FontTextutre") || strstr(msl, "FontTexture")) {
            g_font_ps.insert(so);
            fprintf(stderr, "[FONT] pixel shader obj=%08x identified\n", so);
        }
        // BO2_UI_NOLUTGEN=1 — WORKAROUND, not a fix, for the missing
        // image assets. The frontend's colour-grading LUT is generated
        // from a BASE LUT image, and with no images loaded that base is
        // the engine's 1x1 WHITE placeholder — so the LUT comes out
        // white and the tone-map composite (whose entire output is a LUT
        // sample) washes the whole screen white, hiding the menu that is
        // otherwise drawing correctly underneath. Skipping the generator
        // leaves the LUT at the zero we clear it to, the composite
        // resolves to black, and the UI becomes visible.
        // Identify it the same way as the font shader: by what its MSL
        // declares.
        // THE FOG SHADERS. The floor and the far wall are painted by
        // pixel shaders whose whole output is
        //   sqrt(lerp(COLOR1, tex*scaleRGB, TEXCOORD0.z) * hdrControl0.x)
        // where COLOR1 is the fog COLOUR and TEXCOORD0.z the fog
        // FACTOR, both computed by the vertex shader out of
        // PerSceneConsts.fogColor / fogConsts / fogConsts2 /
        // sunFogColor. A factor of 0 outputs pure fog colour, which is
        // exactly what a washed-out floor looks like. Record the real
        // byte offsets FROM THE TRANSLATOR — computing them by hand off
        // the struct is the trap this project has a rule for.
        if (slot != 9 && strstr(msl, "fogConsts") &&
            strstr(msl, "sunFogColor")) {
            char* lay = nullptr;
            if (bo2_dxbc_cbuffer_layout(it->second.data(),
                                        (uint32_t)it->second.size(),
                                        "PerSceneConsts", &lay) && lay) {
                auto& m = g_cb_layout[so];
                for (const char* q = lay; *q; ) {
                    const char* e = strchr(q, '=');
                    if (!e) break;
                    const char* c = strchr(e, ',');
                    m[std::string(q, (size_t)(e - q))] =
                        (uint32_t)strtoul(e + 1, nullptr, 10);
                    if (!c) break;
                    q = c + 1;
                }
                g_fog_vs.insert(so);
                static int nfl = 0;
                if (++nfl <= 2)
                    fprintf(stderr, "[FOG] vertex shader %08x: PerSceneConsts "
                                    "has %zu vars, fogColor@%u fogConsts@%u "
                                    "fogConsts2@%u sunFogColor@%u\n",
                            so, m.size(), m.count("fogColor") ? m["fogColor"] : 0u,
                            m.count("fogConsts") ? m["fogConsts"] : 0u,
                            m.count("fogConsts2") ? m["fogConsts2"] : 0u,
                            m.count("sunFogColor") ? m["sunFogColor"] : 0u);
                bo2_msl_free(lay);
            }
        }
        if (strstr(msl, "baseLut2D")) {
            g_lutgen_ps.insert(so);
            fprintf(stderr, "[LUT] generator pixel shader obj=%08x "
                            "identified\n", so);
        }
        // THE CONSUMER, by its arithmetic and not by a name — see the
        // note on g_lutuse_ps. Record the declared texture name per
        // Metal index at the same time: the composite declares
        // codeTexture0/1/2 and HLSLcc hands them texture(1)/(2)/(0),
        // so a log that prints only indices cannot show the very
        // mismatch we are hunting.
        if (slot == 9 && strstr(msl, "0.0009765625") &&
            strstr(msl, "0.96875")) {
            g_lutuse_ps.insert(so);
            fprintf(stderr, "[LUT] tone-map composite obj=%08x identified "
                            "(1/1024 and 31/32 both present)\n", so);
        }
        {   // "texture2d<float, access::sample > NAME [[ texture(N) ]]"
            std::vector<std::string>& nm = g_tex_names[so];
            for (const char* q = msl; (q = strstr(q, "[[ texture(")); ) {
                unsigned idx = 0;
                if (sscanf(q + 11, "%u", &idx) == 1 && idx < 32) {
                    // The name is the identifier just before the "[[".
                    const char* e = q;
                    while (e > msl && (e[-1] == ' ' || e[-1] == '\t')) e--;
                    const char* b = e;
                    while (b > msl && (isalnum((unsigned char)b[-1]) ||
                                       b[-1] == '_')) b--;
                    if (nm.size() <= idx) nm.resize(idx + 1);
                    nm[idx].assign(b, (size_t)(e - b));
                }
                q += 11;
            }
        }
        char* attrs = bo2_msl_attributes(msl);
        gfx_register_shader2(so, slot == 9, msl, attrs);
        if (attrs) free(attrs);
        // HLSLcc renumbers BOTH textures and constant buffers from 0 in
        // declaration order, so neither Metal index is the D3D register.
        // Publish both mappings. The constant-buffer one is the one that
        // matters most: the vertex transform lives in a constant buffer,
        // and a shader that reads b0 and b2 gets buffer(0) and buffer(1).
        if (tmap) {
            gfx_register_texslots(so, tmap);
            static uint32_t ntm = 0;
            if (++ntm <= 5)
                fprintf(stderr, "[MSL] texture slots for %08x: %s\n", so, tmap);
            bo2_msl_free(tmap);
        }
        // The DECLARED TEXTURE TYPE per slot. Without it every slot
        // gets a 2D texture, including the texture3d / texturecube /
        // depth2d ones the 3D material shaders declare.
        if (tdim) {
            gfx_register_texdims(so, tdim);
            static uint32_t ntd = 0;
            if (++ntd <= 5)
                fprintf(stderr, "[MSL] texture dims for %08x: %s "
                                "(2=2D 3=3D 4=cube 5=depth)\n", so, tdim);
            bo2_msl_free(tdim);
        }
        if (smap) {                  // P805 : Metal sampler index -> D3D s register
            if (slot == 9) gfx_register_smpslots(so, smap);
            static uint32_t nsm = 0;
            if (++nsm <= 5)
                fprintf(stderr, "[MSL] sampler slots for %08x: %s\n", so, smap);
            bo2_msl_free(smap);
        }
        if (cbmap) {
            gfx_register_cbslots(so, cbmap);
            static uint32_t ncm = 0;
            int identity = 1;
            for (const char* q = cbmap; *q; ) {   // "m=d,m=d,..."
                int m = atoi(q); const char* e = strchr(q, '=');
                if (!e) break;
                if (m != atoi(e + 1)) identity = 0;
                q = strchr(e, ','); if (!q) break; q++;
            }
            if (++ncm <= 8 || (!identity && ncm <= 60))
                fprintf(stderr, "[MSL] cbuffer slots for %08x: %s%s\n", so,
                        cbmap, identity ? "" : "   <-- RENUMBERED");
            bo2_msl_free(cbmap);
        }
        bo2_msl_free(msl);
        nok++;
        if (nok <= 3 || nok % 50 == 0)
            fprintf(stderr, "[MSL] translated on bind: %u ok, %u failed\n",
                    nok, nbad);
    } else if (++nbad <= 5) {
        fprintf(stderr, "[MSL] FAILED to translate a bound %s (%zu bytes)\n",
                slot == 9 ? "PS" : "VS", it->second.size());
    }
    if (slot != 9) vs_reads_vertex_id(so);   // memo avant d'effacer : VSSetShader le relit
                it->second.clear();                 // bytecode no longer needed
}

extern "C" bool d3d_vtbl_dispatch(State& s, uint32_t addr) {
    if ((addr & 0xFF000000u) != D3D_VTBL_BASE) return false;
    int iface = (int)((addr >> 8) & 0xFF);
    int slot  = (int)(addr & 0xFF);
    auto lk = d3d_lock(iface, slot);   // P651 : tout ce qui suit est sous le verrou (sauf l'attente de Present, P774)
    uint32_t esp = s.gpr.rsp.dword;
    auto arg = [&](int i) { return win32_mem_read32(esp + 4 + i * 4); };  // arg0 = this
    if (const int tid = win32_current_tid(); iface < 16 && tid >= 0 && tid < 32 &&
                                             !(g_d3d_tids[iface][slot] & (1u << tid))) {
        g_d3d_tids[iface][slot] |= 1u << tid;
        fprintf(stderr, "[D3D-TID] if=%d slot=%d tid=%d ret=0x%08x\n",
                iface, slot, tid, win32_mem_read32(esp));
    }
    if (g_dynref_on)
        dynref_note(iface, slot, arg(0), arg(1), arg(2), arg(3), win32_mem_read32(esp));
    auto ret = [&](uint32_t rax, int nargs) {
        s.gpr.rax.dword = rax;
        s.gpr.rsp.dword = esp + 4 + (uint32_t)nargs * 4;   // stdcall: pop ret + args
    };
    // IUnknown — QueryInterface(this,riid,ppv). The renderer QIs the D3D11
    // device for IDXGIDevice(1) then walks GetParent->adapter->factory. Slot 6
    // is GetParent in every DXGI layout but CreateTexture3D in ID3D11Device's,
    // so answering QI with the same device object made GetParent dispatch as a
    // create call (arity 4 vs 3) — the ESP drift surfaced 64k calls later as
    // EBP=-0x1a at sub_6bce50's ShowWindow. Hand out a DEDICATED dxgi-device
    // object for those riids; every other riid keeps same-object semantics.
    if (slot == 0) {
        uint32_t riid = arg(1), ppv = arg(2), obj = arg(0);
        if (riid) {
            uint32_t d1 = win32_mem_read32(riid);
            if (d1 == 0x54ec77fau || d1 == 0x77db970fu)   // IID_IDXGIDevice / Device1
                obj = d3d_iface_obj(D3D_IF_DXGIDEV);
        }
        if (ppv) win32_mem_write32(ppv, obj);
        if (ppv && obj == arg(0)) dref_add(obj);   // QI reussi = AddRef (dref_release)
        ret(0, 3); return true;
    }
    // AddRef / Release. These used to be no-ops that always answered 1, so a
    // resource's staging buffer was never given back — and once the WebM video
    // started decoding, the guest created a DYNAMIC TEXTURE PER FRAME and each
    // one kept its 1.38 MB backing forever. 473 live blocks, 624 MiB, and the
    // game then puts up its own "Out of memory error" and exits. Refcount for
    // real, and hand the backing back at zero. Only dynamic textures are tracked
    // — everything else is a fixed-size interface object and costs nothing.
    // ⚠️ AddRef / Release STAY NO-OPS, AND THAT LEAKS — recorded deliberately.
    // Once the WebM video decodes, the guest creates a DYNAMIC TEXTURE PER FRAME
    // and each keeps its 1.38 MB staging backing forever: measured 473 live
    // blocks / 624 MiB, after which the game puts up its own "Out of memory
    // error" and exits. The obvious fix — refcount here and free the backing at
    // zero — WAS TRIED, TWICE, AND BREAKS THE GAME: first counting every shim
    // object, then only the dynamic textures we register with a count of 1. Both
    // leave the loader stuck at 377 images on a black screen, because the guest
    // relies on references D3D itself takes (binding a resource to the context
    // AddRefs it) that never pass through this shim, so our zero is not the
    // real zero and the backing is recycled underneath live data.
    // ⇒ A correct fix has to model the references the RUNTIME holds, not just
    // the ones the game makes.
    // ✅ 2026-09-24 : c'est fait pour les textures dynamiques et leurs vues
    // (dref_release), et SEULEMENT pour liberer leur copie Metal — le tampon invite
    // reste a l'eviction par age. Menu, 240 s de video : 5 250 textures, metal
    // plat a 0,88 Go (12,8 Go avant), aucun dessin sur une texture rendue.
    // La valeur rendue au jeu reste 1.
    // Les textures dynamiques et leurs vues sont maintenant COMPTEES (dref_release),
    // pour la copie Metal seulement ; la valeur rendue au jeu ne change pas.
    if (slot == 1) { dref_add(arg(0));     ret(1, 1); return true; }   // AddRef
    if (slot == 2) { dref_release(arg(0)); ret(1, 1); return true; }   // Release

    // IDXGIObject prelude shared by every DXGI interface (factory/adapter/
    // output/swapchain/dxgi-device): 3 SetPrivateData(4), 4 SetPrivateData-
    // Interface(3), 5 GetPrivateData(4), 6 GetParent(this,riid,pp). D3D11
    // interfaces use these slots differently (device 3-10 = Create*), so this
    // must only match the DXGI interface ids.
    if (iface == D3D_IF_FACTORY || iface == D3D_IF_ADAPTER ||
        iface == D3D_IF_OUTPUT  || iface == D3D_IF_SWAPCHAIN ||
        iface == D3D_IF_DXGIDEV) {
        if (slot == 3) { ret(0, 4); return true; }
        if (slot == 4) { ret(0, 3); return true; }
        if (slot == 5) { ret(0, 4); return true; }
        if (slot == 6) {
            uint32_t pp = arg(2);
            uint32_t parent =
                iface == D3D_IF_DXGIDEV   ? d3d_iface_obj(D3D_IF_ADAPTER) :
                iface == D3D_IF_ADAPTER   ? d3d_iface_obj(D3D_IF_FACTORY) :
                iface == D3D_IF_SWAPCHAIN ? d3d_iface_obj(D3D_IF_FACTORY) :
                iface == D3D_IF_OUTPUT    ? d3d_iface_obj(D3D_IF_ADAPTER) : 0;
            if (pp) win32_mem_write32(pp, parent);
            ret(0, 3); return true;
        }
    }
    // IDXGIDevice(1) methods past the object prelude: 7 GetAdapter, 8 Create-
    // Surface, 9 QueryResourceResidency, 10/11 Set/GetGPUThreadPriority,
    // 12/13 Set/GetMaximumFrameLatency.
    if (iface == D3D_IF_DXGIDEV) {
        switch (slot) {
            case 7:  { uint32_t pp = arg(1);
                       if (pp) win32_mem_write32(pp, d3d_iface_obj(D3D_IF_ADAPTER));
                       ret(0, 2); return true; }
            case 8:  ret(0, 6); return true;
            case 9:  ret(0, 4); return true;
            case 10: ret(0, 2); return true;
            case 11: { uint32_t pp = arg(1);
                       if (pp) win32_mem_write32(pp, 0);
                       ret(0, 2); return true; }
            case 12: ret(0, 2); return true;
            case 13: { uint32_t pp = arg(1);
                       if (pp) win32_mem_write32(pp, 1);
                       ret(0, 2); return true; }
            default: break;
        }
    }
    // IDXGIFactory(1) methods: 7 EnumAdapters (same semantics as EnumAdapters1
    // below), 8 MakeWindowAssociation, 9 GetWindowAssociation, 10 Create-
    // SwapChain, 11 CreateSoftwareAdapter, 13 IsCurrent.
    if (iface == D3D_IF_FACTORY) {
        switch (slot) {
            case 8:  ret(0, 3); return true;
            case 9:  { uint32_t pp = arg(1);
                       if (pp) win32_mem_write32(pp, 0);
                       ret(0, 2); return true; }
            case 10: { uint32_t pp = arg(3);
                       if (pp) win32_mem_write32(pp, d3d_iface_obj(D3D_IF_SWAPCHAIN));
                       ret(0, 4); return true; }
            case 11: { uint32_t pp = arg(2);
                       if (pp) win32_mem_write32(pp, d3d_iface_obj(D3D_IF_ADAPTER));
                       ret(0, 3); return true; }
            case 13: ret(1, 1); return true;
            default: break;
        }
    }
    // IDXGISwapChain::GetDevice(this,riid,ppDevice) — slot 7.
    if (iface == D3D_IF_SWAPCHAIN && slot == 7) {
        uint32_t pp = arg(2);
        if (pp) win32_mem_write32(pp, d3d_iface_obj(D3D_IF_DEVICE));
        ret(0, 3); return true;
    }

    // IDXGIFactory::EnumAdapters (slot 7) / IDXGIFactory1::EnumAdapters1
    // (slot 12) — (this, index, ppAdapter), one adapter either way.
    if (iface == D3D_IF_FACTORY && (slot == 7 || slot == 12)) {
        uint32_t index = arg(1), ppA = arg(2);
        if (index == 0) { if (ppA) win32_mem_write32(ppA, d3d_iface_obj(D3D_IF_ADAPTER)); ret(0, 3); }
        else            { ret(0x887A0002u, 3); }              // DXGI_ERROR_NOT_FOUND
        return true;
    }
    // IDXGIAdapter::EnumOutputs(this, index, ppOutput) — slot 7. One output.
    if (iface == D3D_IF_ADAPTER && slot == 7) {
        uint32_t index = arg(1), ppO = arg(2);
        if (index == 0) { if (ppO) win32_mem_write32(ppO, d3d_iface_obj(D3D_IF_OUTPUT)); ret(0, 3); }
        else            { ret(0x887A0002u, 3); }              // DXGI_ERROR_NOT_FOUND
        return true;
    }
    // IDXGIAdapter(1)::GetDesc(this,pDesc) slot 8 / GetDesc1(this,pDesc1) slot 10.
    // The renderer reads pDesc->Description (WCHAR[128]) and wcstombs's it, then
    // uses the VRAM fields to size pools. Fill a plausible DXGI_ADAPTER_DESC(1):
    //   Description[128] WCHAR @0, VendorId @256, DeviceId @260, SubSysId @264,
    //   Revision @268, DedicatedVideoMemory @272, DedicatedSystemMemory @276,
    //   SharedSystemMemory @280, AdapterLuid @284(8), [DESC1: Flags @292].
    if (iface == D3D_IF_ADAPTER && (slot == 8 || slot == 10)) {
        uint32_t d = arg(1);
        if (d) {
            static const char name[] = "Apple Metal (BO2 shim)";
            for (int i = 0; i < 128; i++)
                win32_mem_write16(d + i * 2, (uint16_t)(i < (int)sizeof(name) ? (uint8_t)name[i] : 0));
            win32_mem_write32(d + 256, 0x106Bu);        // VendorId (Apple)
            win32_mem_write32(d + 260, 0x0001u);        // DeviceId
            win32_mem_write32(d + 264, 0u);             // SubSysId
            win32_mem_write32(d + 268, 0u);             // Revision
            win32_mem_write32(d + 272, 0x80000000u);    // DedicatedVideoMemory = 2GB
            win32_mem_write32(d + 276, 0u);             // DedicatedSystemMemory
            win32_mem_write32(d + 280, 0x40000000u);    // SharedSystemMemory = 1GB
            win32_mem_write32(d + 284, 1u);             // AdapterLuid.LowPart
            win32_mem_write32(d + 288, 0u);             // AdapterLuid.HighPart
            if (slot == 10) win32_mem_write32(d + 292, 0u);   // DESC1 Flags
        }
        ret(0, 2);
        return true;
    }

    // IDXGIOutput::GetDesc(this, pDesc) — slot 7. DXGI_OUTPUT_DESC:
    //   DeviceName[32] WCHAR @0, RECT DesktopCoordinates @64 (l,t,r,b),
    //   BOOL AttachedToDesktop @80, Rotation @84, HMONITOR Monitor @88.
    if (iface == D3D_IF_OUTPUT && slot == 7) {
        uint32_t d = arg(1);
        if (d) {
            static const char dn[] = "\\\\.\\DISPLAY1";
            for (int i = 0; i < 32; i++)
                win32_mem_write16(d + i * 2, (uint16_t)(i < (int)sizeof(dn) ? (uint8_t)dn[i] : 0));
            win32_mem_write32(d + 64, 0);        // left
            win32_mem_write32(d + 68, 0);        // top
            win32_mem_write32(d + 72, kGuestMonitorW);   // right
            win32_mem_write32(d + 76, kGuestMonitorH);   // bottom
            win32_mem_write32(d + 80, 1);        // AttachedToDesktop
            win32_mem_write32(d + 84, 1);        // Rotation = IDENTITY
            win32_mem_write32(d + 88, ++t_fake_handle_counter);   // HMONITOR
        }
        ret(0, 2);
        return true;
    }
    // IDXGIOutput::GetDisplayModeList(this, EnumFormat, Flags, pNumModes, pDesc)
    // — slot 8. First call pDesc=NULL -> write *pNumModes=count; second call
    // fills pDesc[]. Advertise a single 1920x1080@60 mode. DXGI_MODE_DESC (28B):
    //   Width@0, Height@4, RefreshRate{Num@8,Den@12}, Format@16, Scanline@20, Scaling@24.
    if (iface == D3D_IF_OUTPUT && slot == 8) {
        uint32_t pNum = arg(3), pDesc = arg(4);
        if (pNum) {
            if (!pDesc) { win32_mem_write32(pNum, 1); }   // count query
            else {
                uint32_t n = win32_mem_read32(pNum);
                if (n >= 1) {
                    win32_mem_write32(pDesc + 0, kGuestMonitorW);
                    win32_mem_write32(pDesc + 4, kGuestMonitorH);
                    win32_mem_write32(pDesc + 8, 60);     // RefreshRate.Numerator
                    win32_mem_write32(pDesc + 12, 1);     // RefreshRate.Denominator
                    win32_mem_write32(pDesc + 16, 87);    // DXGI_FORMAT_B8G8R8A8_UNORM
                    win32_mem_write32(pDesc + 20, 0);     // ScanlineOrdering
                    win32_mem_write32(pDesc + 24, 0);     // Scaling
                    win32_mem_write32(pNum, 1);
                }
            }
        }
        ret(0, 5);
        return true;
    }

    // --- render-loop methods (standard D3D11/DXGI vtable layouts) ---
    // Fake COM objects for Create* out-params. Resources (buffers/textures) and
    // views get THEIR OWN interface ids — handing back the device object here
    // made texture->GetType()/GetDesc() dispatch as device create-methods with
    // wrong arities, corrupting the renderer-backend thread's stack (the
    // streaming_temp_image stall). Real GPU backing comes with the Metal layer.
    auto new_res  = [&]() { return d3d_iface_obj(D3D_IF_RESOURCE); };
    auto new_view = [&]() { return d3d_iface_obj(D3D_IF_VIEW); };

    // ID3D11Resource/ID3D11Texture2D/ID3D11Buffer (slots: 3 GetDevice, 4-6
    // Get/Set/SetPrivateDataInterface, 7 GetType, 8/9 Set/GetEvictionPriority,
    // 10 GetDesc).
    if (iface == D3D_IF_RESOURCE) {
        switch (slot) {
            case 3: { uint32_t pp = arg(1);
                      if (pp) win32_mem_write32(pp, d3d_iface_obj(D3D_IF_DEVICE));
                      ret(0, 2); return true; }             // GetDevice
            case 4: ret(0, 4); return true;                 // GetPrivateData
            case 5: ret(0, 4); return true;                 // SetPrivateData
            case 6: ret(0, 3); return true;                 // SetPrivateDataInterface
            case 7: { uint32_t pd = arg(1);
                      if (pd) win32_mem_write32(pd, 3);     // D3D11_RESOURCE_DIMENSION_TEXTURE2D
                      ret(0, 2); return true; }             // GetType (void)
            case 8: ret(0, 2); return true;                 // SetEvictionPriority
            case 9: ret(0, 1); return true;                 // GetEvictionPriority
            case 10: {                                      // GetDesc (texture2d/buffer)
                uint32_t d = arg(1);
                if (d) {
                    for (int i = 0; i < 11; i++) win32_mem_write32(d + i * 4, 0);
                    win32_mem_write32(d + 0, 1920);   // Width
                    win32_mem_write32(d + 4, 1080);   // Height
                    win32_mem_write32(d + 8, 1);      // MipLevels
                    win32_mem_write32(d + 12, 1);     // ArraySize
                    win32_mem_write32(d + 16, 87);    // Format B8G8R8A8_UNORM
                    win32_mem_write32(d + 20, 1);     // SampleDesc.Count
                    win32_mem_write32(d + 32, 0x28);  // BindFlags RT|SR
                }
                ret(0, 2); return true;
            }
            default: break;                                 // fall to loud UNIMPL
        }
    }
    // ID3D11*View (slots: 3 GetDevice, 4-6 privdata, 7 GetResource, 8 GetDesc).
    if (iface == D3D_IF_VIEW) {
        switch (slot) {
            case 3: { uint32_t pp = arg(1);
                      if (pp) win32_mem_write32(pp, d3d_iface_obj(D3D_IF_DEVICE));
                      ret(0, 2); return true; }
            case 4: ret(0, 4); return true;
            case 5: ret(0, 4); return true;
            case 6: ret(0, 3); return true;
            case 7: { uint32_t pp = arg(1);                // GetResource(ppResource)
                      // Return the ACTUAL resource this view was made from. It used
                      // to hand back the shared singleton, so the engine's
                      // CopyResource(dst, src) arrived with dst == src == that
                      // singleton and the final "put the composed frame on screen"
                      // step could not be resolved at all.
                      uint32_t res = 0;
                      if (g_track) {
                          uint32_t self = arg(0);
                          auto r = g_rtv_res.find(self);
                          if (r != g_rtv_res.end()) res = r->second;
                          else { auto v = g_geo_srv.find(self);
                                 if (v != g_geo_srv.end()) res = v->second; }
                      }
                      if (!res) res = d3d_iface_obj(D3D_IF_RESOURCE);
                      if (pp) win32_mem_write32(pp, res);
                      if (pp) dref_add(res);             // GetResource rend une reference
                      ret(0, 2); return true; }
            case 8: { uint32_t d = arg(1);
                      if (d) for (int i = 0; i < 5; i++) win32_mem_write32(d + i * 4, i == 0 ? 87u : 0u);
                      ret(0, 2); return true; }             // GetDesc
            default: break;
        }
    }

    // IDXGISwapChain::Present(this, SyncInterval, Flags) slot 8 — THE FRAME
    // BOUNDARY. Count + log; the renderer having reached repeated Present means
    // it is running its frame loop (a real backbuffer/PNG dump hangs off here
    // once the Metal layer backs GetBuffer's texture).
    if (iface == D3D_IF_SWAPCHAIN && slot == 8) {
        // P773 : sync = SyncInterval. Sous Windows, 1 bloque Present jusqu'au retour de trame (60 img/s sur ce
        // 60 Hz) ; nous rendons la main tout de suite et soumettons jusqu'à 96 img/s, de la chaleur pour rien.
        if (g_present_frames < 20 || g_present_frames % 100 == 0)
            fprintf(stderr, "[D3D-PRESENT] frame %u sync=%u flags=0x%x\n", g_present_frames, arg(1), arg(2));
        // 🎯 BO2_PHYSSTACK=1 — LA PILE D'ALLOCATIONS NOMMEES, FILMEE.
        //
        // L'echec de la barriere rapide est un `Com_Error("free does not match
        // allocation")` : le moteur libere "LUIFrontendAnimationStates" en slot 4
        // alors que "cinematicTextures" occupe encore le slot 5 au-dessus, dans
        // un allocateur LIFO. La sonde [PHYS-FREE-ERROR] montre cet etat AU
        // CRASH ; elle ne dit pas comment on y est arrive, ni ce que fait un run
        // qui REUSSIT au meme endroit.
        //
        // C'est un SONDAGE, pas un crochet : la pile vit a une adresse invitee
        // fixe, donc une lecture par image suffit et ne peut pas perturber ce
        // qu'elle mesure (regle maison : un sondage bat un crochet qui pourrait
        // ne pas etre sur le chemin). On n'imprime que les CHANGEMENTS, donc le
        // journal porte la chronologie complete des empilements/depilements pour
        // le prix de quelques dizaines de lignes.
        if (__builtin_expect(bo2_env_on("BO2_PHYSSTACK"), 0)) {
            static uint32_t last_count = 0xffffffffu, last_top = 0;
            const uint32_t base = 0x2b221e8u;                 // meme base que [PHYS-FREE-ERROR]
            const uint32_t count = win32_mem_read32(base);
            if (count <= 80) {
                const uint32_t top = count ? win32_mem_read32(base + 8 + (count - 1) * 8) : 0;
                if (count != last_count || top != last_top) {
                    last_count = count; last_top = top;
                    char line[512]; int o = 0;
                    o += snprintf(line + o, sizeof line - o,
                                  "[PHYSSTACK] frame=%u count=%u cursor=0x%08x :",
                                  g_present_frames, count, win32_mem_read32(base + 4));
                    for (uint32_t i = 0; i < count && o < 440; ++i) {
                        const uint32_t np = win32_mem_read32(base + 8 + i * 8);
                        char nm[48]; int k = 0;
                        while (k < 47) { uint8_t c = win32_mem_read8(np + k); if (!c) break; nm[k++] = (char)c; }
                        nm[k] = 0;
                        o += snprintf(line + o, sizeof line - o, " %u:%s", i, k ? nm : "(null)");
                    }
                    fprintf(stderr, "%s\n", line);
                }
            }
        }
        // 🎯 BO2_CINEGATE=1 — POURQUOI cinematicTextures N'EST JAMAIS DEPILE.
        //
        // Desassemblage statique de 0x6c9f1b (zero run) : la liberation du bloc
        // est le slot +0x0c de la vtable cinematique, et elle est gardee par
        // QUATRE conditions, toutes dans la meme fonction d'arret :
        //
        //   0x6c9f1b  cmp  [0x37d5ec8], 0 ; je  sortie    -> doit etre NON NUL
        //   0x6c9f23  cmp  [0x37d45c0], 0 ; jne sortie    -> doit etre NUL
        //   0x6c9f2b  test [0x37d47dc], 4 ; jne sortie    -> bit 2 doit etre CLAIR
        //   0x6c9f34  inc  [0x37d5c78] ; cmp 5 ; jl sortie -> CINQ passages
        //   0x6c9f44  mov ecx,[0x37d48f4] ; call [ecx+0xc]  <- LA LIBERATION
        //
        // Ce n'est donc pas un evenement mais un COMPTE A REBOURS : la fonction
        // doit etre atteinte cinq fois de suite avec les trois premieres
        // conditions vraies, sinon le bloc reste sur la pile LIFO et la
        // liberation de LUIFrontendAnimationStates en dessous devient un
        // "free does not match allocation".
        //
        // (historique) Un sondage par image de ces cinq mots dit LAQUELLE des gardes bloque
        // et A QUEL COMPTE le compteur se fige -- ce qu'aucune sonde posee au
        // crash ne peut dire, puisqu'au crash la liberation n'a jamais ete
        // TENTEE. Sondage, pas crochet : aucune interposition, aucun re-lift.
        // ⚠️ ETAPE 51 : les quatre gardes ci-dessus sont RETRACTEES. Elles
        // decrivent le site vtable 0x6c9f1b, pas sub_6cd5e0. La vraie condition
        // de liberation est `bl` : [0x37d5c64] > 0 (une cinematique EN FILE)
        // && [0x37d4b18]&1 && [0x37d4b19]&1. On sonde donc la FILE, parce que
        // la question qui reste est une question de DATE : a quelle seconde la
        // suivante entre-t-elle en file, et a quelle seconde l'invite demande
        // la liberation. L'ecart entre les deux est toute la marge.
        if (__builtin_expect(bo2_env_on("BO2_CINEGATE"), 0)) {
            static uint32_t l_q = 0xffffffffu, l_18 = 0xffffffffu, l_19 = 0xffffffffu,
                            l_b = 0xffffffffu, l_f = 0xffffffffu, l_v = 0xffffffffu;
            const uint32_t q   = win32_mem_read32(0x37d5c64u);  // file d'attente
            const uint32_t b18 = win32_mem_read8 (0x37d4b18u);
            const uint32_t b19 = win32_mem_read8 (0x37d4b19u);
            const uint32_t b   = win32_mem_read32(0x37d45c0u);  // index video courante
            const uint32_t f   = win32_mem_read32(0x37d47dcu);  // bit 2 = travail async
            const uint32_t v   = win32_mem_read32(0x37d48f4u);  // backend (0 = detruit)
            if (q != l_q || b18 != l_18 || b19 != l_19 || b != l_b ||
                (f & 4u) != (l_f & 4u) || v != l_v) {
                l_q = q; l_18 = b18; l_19 = b19; l_b = b; l_f = f; l_v = v;
                const bool bl = ((int32_t)q > 0) && (b18 & 1u) && (b19 & 1u);
                fprintf(stderr,
                        "[CINEGATE] frame=%u file=%d 4b18=0x%02x 4b19=0x%02x "
                        "45c0=%u bit2=%u backend=0x%08x bl=%d %s\n",
                        g_present_frames, (int32_t)q, b18, b19, b, (f >> 2) & 1u, v,
                        (int)bl, bl ? "LIBERERAIT" : "NE LIBERERAIT PAS");
            }
        }
        // BO2_STREAM_PROBE=1 — the IMAGE STREAMING state, read straight out of
        // guest memory (no interpose needed, so it costs nothing and cannot
        // perturb what it measures).
        //   0x292399d  streaming enabled (set once the worker thread starts)
        //   0x29239a0  head of the pending-request list, 0x29239a8 its count
        //   records at 0x291f198, stride 0x120, 40 of them:
        //     +0x000 char name[0x104]   +0x100 handle
        //     +0x104 in-use  +0x105 want-open  +0x106 ?  +0x107 OPEN FAILED
        // FUN_00576b30 refuses to submit a read when +0x107 is set, so a slot
        // flagged failed is a slot that can never stream.
        static const int sprobe = bo2_getenv("BO2_STREAM_PROBE") ? 1 : 0;
        if (sprobe && (g_present_frames % 400) == 1) {
            // The engine's OWN ipak slot array: base 0x3124a90, stride 0x68,
            // name at +0, handle at +0x60, loaded flag at +0x64. The descriptor we
            // write puts a slot index in the top nibble of [img+0x34], and
            // FUN_004cba90 turns that into THIS handle — so if the mapping is off
            // the engine reads the wrong file and the chunk-table walk sees
            // garbage. Print it rather than infer it.
            for (uint32_t q = 0; q < 16; q++) {
                uint32_t rq = 0x3124a90u + q * 0x68u;
                uint32_t loaded = win32_mem_read32(rq + 0x64);
                uint32_t handle = win32_mem_read32(rq + 0x60);
                char sn[72]; int k = 0;
                while (k < 71) { uint8_t c = win32_mem_read8(rq + (uint32_t)k);
                                 if (c < 0x20 || c > 0x7e) break; sn[k++] = (char)c; }
                sn[k] = 0;
                if (!loaded && !sn[0]) continue;
                fprintf(stderr, "[IPAK-SLOT] %2u loaded=%u handle=%d name=\"%s\"\n",
                        q, loaded, (int32_t)handle, sn);
            }
            // WHICH FUNCTION IS EACH THREAD ACTUALLY RUNNING? FUN_0080dee0(id)
            // calls [id*4 + 0x2925668] — a RUNTIME table, so the static thread
            // name table at 0xbe4130 only tells you the name, not the body. This
            // prints the live mapping, which is the only way to see whether
            // DB_DecompressIOStreamWorker (FUN_005849b0) is bound to any thread
            // that actually started.
            {
                static const char* tnames[19] = {
                    "Main","Backend","Worker0","Worker1","Worker2","Worker3",
                    "Worker4","Worker5","Worker6","Worker7","Server","Database",
                    "Stream","Sound Mix","Sound Decode","WebM Decode",
                    "Yuv420 Capture","WebM Encode IO","WebM http" };
                fprintf(stderr, "[THREADS] id -> live entry fn\n");
                for (uint32_t q = 0; q < 19; q++) {
                    uint32_t fn = win32_mem_read32(0x2925668u + q * 4u);
                    if (!fn) continue;
                    fprintf(stderr, "[THREADS]   %2u %-16s fn=%08x%s\n", q, tnames[q],
                            fn, fn == 0x5849b0u ? "   <-- DB_DecompressIOStreamWorker" : "");
                }
            }
            // The 32 streaming_temp_image_N staging images (FUN_006fad90 builds
            // them at renderer init: record at 0x3833200, stride 0xc — +0 image
            // pointer, +4 = -1 marker, +8 state). Every streamed upload needs one,
            // so if this table is empty no image can ever become resident.
            {
                int have = 0;
                for (int q = 0; q < 32; q++)
                    if (win32_mem_read32(0x3833200u + (uint32_t)q * 0xcu)) have++;
                fprintf(stderr, "[STREAM] staging images: %d/32 created; slot0 img=%08x "
                                "mark=%08x state=%08x\n", have,
                        win32_mem_read32(0x3833200u), win32_mem_read32(0x3833204u),
                        win32_mem_read32(0x3833208u));
            }
            // r_noipak — if this dvar is set the engine will not use the image
            // paks at all, which would look EXACTLY like what we see: the ipaks
            // get opened at init and then never read. Its dvar pointer is stored
            // at 0x321aca4 by the registration at 0x69f257; dvar layout is
            // [+0x10]=type, [+0x18]=value.
            {
                // 🪤 THE STORE FOLLOWS THE CALL IT BELONGS TO. In this
                // registration run, `mov [0x321aca4], eax` sits between r_noipak's
                // argument pushes and its call — but it stores the PREVIOUS
                // registration's result (sm_fastSunShadow, default 1). r_noipak's
                // own pointer lands in 0x321ad70. Reading the wrong global made a
                // bool that is 1 by design look like a smoking gun.
                // Dvar_GetBool (FUN_004ba000): type at +0x10 must be 1, value is
                // the BYTE at +0x18.
                // r_loadForRenderer (dvar ptr at 0x321ac5c, registered default TRUE)
                // gates the ENTIRE image->GPU path: FUN_006d0c60 (the per-image
                // residency gate), FUN_006d0bf0 (the streamed load that reads the
                // ipak), FUN_006d0970 (code images) and both Image_Setup callers
                // all bail immediately when it is false. If this is 0 the engine
                // registers every image and creates no texture for any of them,
                // which is exactly the state we measured.
                struct DvarProbe { uint32_t slot; const char* name; };
                static const DvarProbe dvp[] = {
                    { 0x321ac5cu, "r_loadForRenderer" },
                    { 0x321ad70u, "r_noipak" },
                };
                for (const DvarProbe& p2 : dvp) {
                    uint32_t dv = win32_mem_read32(p2.slot);
                    if (dv > 0x1000u && dv < 0xF0000000u)
                        fprintf(stderr, "[STREAM] %s dvar=%08x type=%u value=%u\n",
                                p2.name, dv, win32_mem_read32(dv + 0x10),
                                win32_mem_read8(dv + 0x18));
                    else
                        fprintf(stderr, "[STREAM] %s NOT REGISTERED (%08x)\n",
                                p2.name, dv);
                }
            }
            fprintf(stderr, "[STREAM] enabled=%u pending=%d head=%08x\n",
                    win32_mem_read8(0x292399du),
                    (int32_t)win32_mem_read32(0x29239a8u),
                    win32_mem_read32(0x29239a0u));
            for (uint32_t i = 0; i < 40; i++) {
                uint32_t rec = 0x291f198u + i * 0x120u;
                char nm[64] = {0};
                for (int c = 0; c < 63; c++) {
                    uint8_t ch = win32_mem_read8(rec + (uint32_t)c);
                    if (!ch) break;
                    nm[c] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
                }
                if (!nm[0]) continue;
                fprintf(stderr, "[STREAM]   slot %2u \"%s\" handle=%d inuse=%u "
                                "wantopen=%u b2=%u FAILED=%u\n",
                        i, nm, (int32_t)win32_mem_read32(rec + 0x100),
                        win32_mem_read8(rec + 0x104), win32_mem_read8(rec + 0x105),
                        win32_mem_read8(rec + 0x106), win32_mem_read8(rec + 0x107));
            }
        }
        g_present_frames++;   // progress signal for the no-progress runaway guard
        // Present IS the frame boundary for the live path: hand this frame's UI
        // triangles to the Metal window and start collecting the next one.
        if (g_window_gfx) {
            if (!g_ui_v.empty() && !g_ui_b.empty())
                gfx_ui_frame(g_ui_v.data(), (uint32_t)g_ui_v.size(),
                             g_ui_b.data(), (uint32_t)g_ui_b.size(),
                             g_ui_vpw, g_ui_vph);
            g_ui_v.clear();
            g_ui_b.clear();
            // The frame's draws have been consumed: recycle the ring and let
            // each buffer take a fresh slice next frame.
            g_ui_ring_off = 0;
            g_ui_ring_map.clear();
            gfx_real_frame_end();
        }
        // P773 : sous Windows, Present(SyncInterval ≥ 1) attend le retour de trame : le jeu ne dépasse pas le
        // rafraîchissement de l'écran. Nous rendions la main tout de suite : jusqu'à 96 img/s soumises sur un
        // écran à 60 Hz, du CPU pour des images jamais vues, donc de la chaleur et le bridage du Mac. Intervalle
        // minimal entre deux Present = SyncInterval / rafraîchissement : l'effet d'une file DXGI pleine, sans
        // en imiter la grille (une image en retard ne fait pas attendre la trame suivante).
        if (const uint32_t sync = arg(1)) {
            static uint64_t prochaine = 0, attendu_ns = 0, n = 0;
            const double hz = gfx_refresh_hz();
            const uint64_t periode = (uint64_t)(1e9 * (sync < 4 ? sync : 4) / hz);
            const uint64_t avant = bo2_mono_ns(), echeance = prochaine;
            const uint64_t now = echeance > avant ? echeance : avant;   // fin de l'attente ci-dessous
            attendu_ns += now - avant;
            prochaine = (prochaine && now < prochaine + periode) ? prochaine + periode : now + periode;
            if (++n == 1 || n % 3000 == 0)
                fprintf(stderr, "[PRESENT-SYNC] #%llu SyncInterval=%u écran %.0f Hz période %.2f ms, attendu "
                                "%.1f s en tout\n", (unsigned long long)n, sync, hz, periode / 1e6,
                        attendu_ns / 1e9);
            // P774 : attendre HORS du verrou D3D. En mono-fil, l'attente cède aux autres coroutines du même
            // fil OS ; le fil Database y appelait CreateTexture2D -> std::mutex déjà tenu par ce fil OS ->
            // interblocage dès le chargement de `common`. Sous Windows, Present ne bloque pas le device.
            lk.unlock();
            if (echeance > avant) bo2_attendre_jusqua_ns(echeance);
        }
        ret(0, 3); return true;
    }
    // IDXGISwapChain::GetBuffer(this, Buffer, riid, ppSurface) slot 9 — backbuffer.
    if (iface == D3D_IF_SWAPCHAIN && slot == 9) {
        uint32_t pp = arg(3);
        // A stable object, so the RTV created from it can be recognised as THE
        // backbuffer: only draws aimed at it belong on screen. Everything else the
        // engine renders offscreen and composites, and sending those to the window
        // paints over the UI.
        if (g_track) {
            if (!g_backbuffer_res) {
                uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
                g_backbuffer_res = win32_guest_alloc(64, 4);
                win32_mem_write32(g_backbuffer_res + 0, vt);
                for (int i = 4; i < 64; i += 4) win32_mem_write32(g_backbuffer_res + i, 0);
            }
            if (g_window_gfx) gfx_set_backbuffer_res(g_backbuffer_res);
            if (pp) win32_mem_write32(pp, g_backbuffer_res);
            ret(0, 4); return true;
        }
        if (pp) win32_mem_write32(pp, new_res());
        ret(0, 4); return true;
    }
    // IDXGISwapChain::GetDesc(this,pDesc) slot 12 — DXGI_SWAP_CHAIN_DESC. Fill the
    // fields the renderer reads (BufferDesc mode + BufferCount + OutputWindow).
    if (iface == D3D_IF_SWAPCHAIN && slot == 12) {
        uint32_t d = arg(1);
        if (d) {
            for (int i = 0; i < 28; i++) win32_mem_write32(d + i * 4, 0);
            win32_mem_write32(d + 0, 1920);        // BufferDesc.Width
            win32_mem_write32(d + 4, 1080);        // BufferDesc.Height
            win32_mem_write32(d + 8, 60);          // RefreshRate.Numerator
            win32_mem_write32(d + 12, 1);          // RefreshRate.Denominator
            win32_mem_write32(d + 16, 87);         // Format B8G8R8A8_UNORM
            win32_mem_write32(d + 36, 1);          // SampleDesc.Count
            win32_mem_write32(d + 44, 2);          // BufferCount
        }
        ret(0, 2); return true;
    }
    // IDXGISwapChain::ResizeBuffers slot 13 (6 args) / ResizeTarget slot 14 (2).
    if (iface == D3D_IF_SWAPCHAIN && slot == 13) { ret(0, 6); return true; }
    if (iface == D3D_IF_SWAPCHAIN && slot == 14) { ret(0, 2); return true; }
    // IDXGISwapChain::GetFullscreenState(this,pFS,ppTarget) slot 11 (windowed).
    if (iface == D3D_IF_SWAPCHAIN && slot == 11) {
        uint32_t pFS = arg(1), ppT = arg(2);
        if (pFS) win32_mem_write32(pFS, 0);
        if (ppT) win32_mem_write32(ppT, 0);
        ret(0, 3); return true;
    }
    // IDXGISwapChain::GetContainingOutput(this,ppOutput) slot 15.
    if (iface == D3D_IF_SWAPCHAIN && slot == 15) {
        uint32_t ppO = arg(1);
        if (ppO) win32_mem_write32(ppO, d3d_iface_obj(D3D_IF_OUTPUT));
        ret(0, 2); return true;
    }
    // ID3D11Device resource/view creation, slots 3..10 (CreateBuffer,
    // CreateTexture1D/2D/3D, Create{ShaderResource,UnorderedAccess,RenderTarget,
    // DepthStencil}View) — all 4 args, out ppObject = arg3. Slots 3-6 make
    // resources, 7-10 make views (distinct fake interfaces — see above).
    if (iface == D3D_IF_DEVICE && slot >= 3 && slot <= 10) {
        // GEOMETRY CAPTURE: give CreateBuffer (slot 3) a UNIQUE object (sharing
        // the RESOURCE vtable so method dispatch is unchanged) + its own backing,
        // seeded with pInitialData (static index/vertex buffers carry their data
        // at creation). Map() then returns this buffer's backing, not the shared
        // scratch, so VB/IB survive intact to Draw time.
        if (g_track && slot == 3) {
            uint32_t pDesc = arg(1), pInit = arg(2), pp = arg(3);
            uint32_t width = pDesc ? win32_mem_read32(pDesc + 0) : 0;
            uint32_t bind  = pDesc ? win32_mem_read32(pDesc + 8) : 0;
            uint32_t vt  = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));  // shared RESOURCE vtable
            uint32_t obj = win32_guest_alloc(64, 4);
            win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            // Only a buffer created WITH initial data needs storage of its own:
            // those are the static index/vertex buffers, and there are few. The
            // frontend creates THOUSANDS of dynamic buffers (measured: 4000+,
            // 67 MB of backings), and since guest memory is a byte-granular hash
            // map, giving each one its own allocation made every memory access
            // slower until the stall watchdog fired — which read as "tid4 is
            // spinning in the 2D pass". Dynamic buffers get a ring slice at Map
            // time instead (see the Map handler), which is bounded and coherent.
            static uint64_t backing_bytes = 0;
            uint32_t backing = 0;
            if (width && pInit && win32_mem_read32(pInit + 0)) {
                backing = win32_guest_alloc(width, 16);
                if (backing) backing_bytes += width;
            }
            {   static uint32_t nbuf = 0; nbuf++;
                if (nbuf <= 4 || nbuf % 4000 == 0)
                    fprintf(stderr, "[UI-BACKING] CreateBuffer #%u size=%u bind=%u "
                                    "static=%lluMB\n", nbuf, width, bind,
                            (unsigned long long)(backing_bytes >> 20));
            }
            if (pInit && backing) {
                uint32_t src = win32_mem_read32(pInit + 0);
                if (src) for (uint32_t i = 0; i < width; i += 4)
                    win32_mem_write32(backing + i, win32_mem_read32(src + i));
            }
            // Buffers are created every frame by the UI; without a bound the map
            // grows without limit over a long run.
            g_geo_bufs[obj] = { backing, width, bind };
            g_d3d_refs[obj] = 1;                // P1049 : la reference que Create rend au jeu
            tampons_morts_rendre();
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 4); return true;
        }
        // CreateTexture2D with real fastfile pixels — hand them to the Metal
        // window (BO2_WINDOW) as well as the offline capture. This is the live
        // path that puts genuine BO2 art on screen: the guest already decodes
        // and uploads the real images, we simply stop throwing them away.
        // Independent of BO2_GEO_DUMP so the window works without the capture's
        // per-object buffer backing.
        if (g_window_gfx && slot == 5) {
            uint32_t d = arg(1), pInit = arg(2);
            uint32_t w = d ? win32_mem_read32(d + 0) : 0, h = d ? win32_mem_read32(d + 4) : 0;
            uint32_t fmt = d ? win32_mem_read32(d + 16) : 0;
            uint32_t dataVA = 0, pitch = 0;
            if (pInit) { dataVA = win32_mem_read32(pInit + 0); pitch = win32_mem_read32(pInit + 4); }
            if (dataVA && w && h && w <= 8192 && h <= 8192) {
                uint32_t bytes = pitch ? pitch * ((fmt >= 70 && fmt <= 84) ? (h + 3) / 4 : h)
                                       : w * h * 4;
                if (bytes && bytes <= (32u << 20)) {
                    std::vector<uint8_t> tmp(bytes);
                    win32_mem_read_block(tmp.data(), dataVA, bytes);
                    extern void gfx_stage_texture(uint32_t, uint32_t, uint32_t,
                                                  const void*, uint32_t, uint32_t);
                    gfx_stage_texture(w, h, fmt, tmp.data(), bytes, pitch);
                }
            }
            // fall through to the capture path below when it is also enabled
        }
        // CreateTexture2D(pDesc, pInitialData[], ppTexture) — capture the image.
        // CreateTexture1D (slot 4) and CreateTexture3D (slot 6) fell through to
        // new_res(), which is a SINGLETON — every 1D/3D texture in the game was
        // the same object, so an SRV made from one resolved to whichever texture
        // last claimed that id. The frontend's colour-grading LUT is one of these,
        // and the tone-map composite's entire output is a LUT sample, so a
        // mis-resolved LUT paints the whole screen one flat colour.
        if (g_track && (slot == 4 || slot == 6)) {
            uint32_t d = arg(1), pInit = arg(2), pp = arg(3);
            uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
            uint32_t obj = win32_guest_alloc(64, 4); win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            // 1D: Width 0, MipLevels 4, ArraySize 8, Format 12.
            // 3D: Width 0, Height 4, Depth 8, MipLevels 12, Format 16.
            uint32_t w  = d ? win32_mem_read32(d + 0) : 0;
            uint32_t h  = (slot == 6 && d) ? win32_mem_read32(d + 4) : 1;
            uint32_t dp = (slot == 6 && d) ? win32_mem_read32(d + 8) : 1;
            uint32_t fm = d ? win32_mem_read32(d + (slot == 6 ? 16 : 12)) : 0;
            if (g_window_gfx) {
                static uint32_t n13 = 0;
                if (++n13 <= 16) {
                    // THE WHOLE DESCRIPTOR, not just the dimensions. "Created with
                    // no initial data and never written by any context call" has
                    // exactly three innocent readings — it is a render target, it
                    // is dynamic and Mapped, or it is immutable and we misread
                    // pInitialData — and Usage/BindFlags/CPUAccess/MiscFlags
                    // distinguish them without a single guess.
                    // D3D11_TEXTURE3D_DESC: W 0, H 4, D 8, Mips 12, Format 16,
                    // Usage 20, BindFlags 24, CPUAccess 28, Misc 32.
                    fprintf(stderr, "[gfx] Texture%dD obj=%08x %ux%ux%u fmt=%u data=%s"
                                    "  mips=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x\n",
                            slot == 4 ? 1 : 3, obj, w, h, dp, fm, pInit ? "yes" : "no",
                            d ? win32_mem_read32(d + 12) : 0,
                            d ? win32_mem_read32(d + 20) : 0,
                            d ? win32_mem_read32(d + 24) : 0,
                            d ? win32_mem_read32(d + 28) : 0,
                            d ? win32_mem_read32(d + 32) : 0);
                }
            }
            // UPLOAD A 3D TEXTURE'S PIXELS. This never happened before: the object
            // was created per-instance (fixing the old singleton) but its contents
            // were dropped on the floor, so `texture3d modelLightingSampler` — the
            // baked lighting volume the whole room is lit by — could only ever
            // resolve to a stand-in. D3D11_SUBRESOURCE_DATA is
            // { pSysMem +0, SysMemPitch +4, SysMemSlicePitch +8 }.
            // Remember the descriptor: a 3D texture created with NO initial data is
            // filled later through UpdateSubresource, and that call carries only a
            // resource pointer — the dimensions have to come from here.
            if (slot == 6 && w && h && dp > 1) {
                g_tex3d_desc[obj] = { w, h, dp, fm };
                if (d && !(win32_mem_read32(d + 24) & 0xE0u)) g_d3d_refs.try_emplace(obj, 1u);   // P1051
                // usage 2 = DYNAMIC, 3 = STAGING, or CPU_ACCESS_WRITE — all three
                // mean "filled through Map/Unmap", which the descriptor says this
                // one is (usage=2 bind=0x8 cpu=0x10000).
                uint32_t usg = d ? win32_mem_read32(d + 20) : 0;
                uint32_t cpu = d ? win32_mem_read32(d + 28) : 0;
                if (g_window_gfx && (usg == 2 || usg == 3 || (cpu & 0x10000u))) {
                    dyntex_register_vol(obj, w, h, dp, fm);
                    dynref_created(obj, win32_mem_read32(esp));
                }
            }
            if (g_window_gfx && slot == 6 && pInit && w && h && dp > 1 &&
                w <= 4096 && h <= 4096 && dp <= 512) {
                uint32_t dataVA = win32_mem_read32(pInit + 0);
                uint32_t pitch  = win32_mem_read32(pInit + 4);
                uint32_t sp     = win32_mem_read32(pInit + 8);
                if (!sp) sp = pitch * h;
                uint64_t nb = (uint64_t)sp * dp;
                if (dataVA && nb && nb <= (32u << 20)) {
                    std::vector<uint8_t> tb((size_t)nb);
                    win32_mem_read_block(tb.data(), dataVA, (uint32_t)nb);
                    gfx_register_texture_vol(obj, w, h, dp, 1, fm, tb.data(),
                                             (uint32_t)nb, pitch, sp);
                }
            }
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 4); return true;
        }
        if (g_track && slot == 5) {
            uint32_t d = arg(1), pInit = arg(2), pp = arg(3);
            uint32_t w = d ? win32_mem_read32(d + 0) : 0, h = d ? win32_mem_read32(d + 4) : 0;
            uint32_t fmt = d ? win32_mem_read32(d + 16) : 0;
            uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
            uint32_t obj = win32_guest_alloc(64, 4); win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            uint32_t dataVA = 0, pitch = 0;
            if (pInit) { dataVA = win32_mem_read32(pInit + 0); pitch = win32_mem_read32(pInit + 4); }
            if (g_window_gfx) {
                static uint32_t nt = 0;
                if (++nt <= 40)
                    fprintf(stderr, "[gfx] Texture2D obj=%08x %ux%u fmt=%u bind=%08x "
                                    "data=%s\n", obj, w, h, fmt,
                            d ? win32_mem_read32(d + 32) : 0, dataVA ? "yes" : "no");
                // WHO creates a texture? CreateTexture2D arrives through the fake
                // COM vtable, so a direct-call scan cannot find the caller — but
                // the guest RETURN ADDRESS is right there at [esp]. Naming the
                // five call sites that DO produce real images tells us which path
                // works, and therefore which path the other 1733 images are
                // missing.
                if (dataVA && w > 4 && h > 4)
                    fprintf(stderr, "[gfx] REAL IMAGE %ux%u fmt=%u from guest ret=%08x\n",
                            w, h, fmt, win32_mem_read32(esp));
                // ARE THE GAME'S IMAGES ACTUALLY LOADING? The frontend substitutes
                // a 1x1 "code" texture (white/black/grey) wherever an image asset
                // is missing, and those placeholders are indistinguishable from
                // real art at the binding level. The tone-map's BASE LUT resolving
                // to the 1x1 white one is what turns the whole screen white, so
                // count real images versus placeholders directly.
                static uint32_t n_tiny = 0, n_real = 0, n_notex = 0;
                if (dataVA && w <= 4 && h <= 4)      n_tiny++;
                else if (dataVA)                     n_real++;
                else                                 n_notex++;
                if (((n_tiny + n_real + n_notex) % 50) == 1)
                    fprintf(stderr, "[gfx] textures created: %u REAL images (>4px with "
                                    "pixels), %u 1x1-ish placeholders, %u with no "
                                    "initial data (render targets etc)\n",
                            n_real, n_tiny, n_notex);
            }
            if (g_geo) geo_dump_tex(obj, w, h, fmt, dataVA, pitch);
            // D3D11_TEXTURE2D_DESC: Width 0, Height 4, MipLevels 8, ArraySize 12,
            // Format 16, SampleDesc 20/24, Usage 28, BindFlags 32.
            // BIND_RENDER_TARGET = 0x20. The frontend composites its menu out of
            // these, so they have to become real MTLTextures we can render into.
            if (g_window_gfx && d) {
                uint32_t bind = win32_mem_read32(d + 32);
                // 0x20 = RENDER_TARGET, 0x40 = DEPTH_STENCIL. Depth targets are
                // SAMPLED as shader resources too (bind=0x48): the 1024x1024
                // shadow map is read 1.4 million times a run, and without a real
                // texture behind it every one of those reads hit the stand-in.
                if ((bind & 0x60u) && w && h && w <= 8192 && h <= 8192)
                    gfx_register_rendertarget(obj, w, h, fmt, win32_mem_read32(d + 8));
            }
            // 🎯 P1051 : une image de niveau est comptee comme une texture dynamique, sinon sa copie Metal reste a vie
            // (55 000 textures et 10 Go apres la campagne). Hors RT/DS/UAV : leurs liaisons OM ne sont pas modelisees.
            if (d && !(win32_mem_read32(d + 32) & 0xE0u)) g_d3d_refs.try_emplace(obj, 1u);
            // DYNAMIC TEXTURES — the ones the guest fills with Map/Unmap rather
            // than at creation. This is how BO2 delivers DECODED VIDEO: the WebM
            // path (libvpx VP8, compiled into the exe) writes each frame's Y, Cr
            // and Cb planes into dynamic textures, and the frontend's cinematic
            // shader samples them. Our Map only ever handed back a shared scratch
            // buffer, so every decoded frame was written into scratch and thrown
            // away — which is why those planes stay 1x1 placeholders and the menu
            // backdrop is black. Record the ones we must be able to upload later.
            // D3D11_USAGE_DYNAMIC = 2 at desc+28; CPU_ACCESS_WRITE = 0x10000 at
            // desc+36. Match on either, because a staging texture (usage 3) is
            // filled the same way.
            if (g_window_gfx && d && w && h && w <= 8192 && h <= 8192) {
                uint32_t usage = win32_mem_read32(d + 28);
                uint32_t cpu   = win32_mem_read32(d + 36);
                if (usage == 2 || usage == 3 || (cpu & 0x10000u)) {
                    dyntex_register(obj, w, h, fmt);
                    dynref_created(obj, win32_mem_read32(esp));
                }
            }
            // A CUBE MAP arrives here, not at CreateTexture3D: D3D expresses one
            // as a 2D texture with ArraySize 6 (desc+12) and MiscFlags
            // D3D11_RESOURCE_MISC_TEXTURECUBE = 0x4 (desc+40). The reflection
            // probe the room's materials sample is one of these, and treating it
            // as a plain 2D texture meant `texturecube reflectionProbeSampler`
            // could only resolve to a stand-in. pInitialData is then an ARRAY of
            // six D3D11_SUBRESOURCE_DATA, one per face, so the faces are read
            // through their own descriptors rather than assumed contiguous.
            // ⚠️ COUNT THE CUBES THIS GUARD TURNS AWAY. `pInit` excludes every
            // cube created WITHOUT initial data — i.e. every dynamic reflection
            // probe, which is rendered at runtime rather than shipped in the
            // fastfile. Those are exactly the slots still reporting "wrong type,
            // 0 had a real texture bound" (942,353 of them on afghanistan), so
            // say how many and how big before assuming the gate is harmless.
            // ⚠️ THE pInit HYPOTHESIS WAS WRONG — measured 0 cubes created without
            // initial data. So log EVERY 6-slice creation regardless of MiscFlags
            // and of pInit, plus the resource id, so the ones the shader later
            // wants as `texturecube` can be matched against what we registered.
            if (g_window_gfx && d && w && h) {
                uint32_t arr_ = win32_mem_read32(d + 12);
                if (arr_ == 6) {
                    static uint32_t nc6 = 0;
                    if (++nc6 <= 24)
                        fprintf(stderr, "[gfx] SIX-SLICE #%u obj=%08x %ux%u fmt=%u mips=%u "
                                        "misc=0x%x pInit=%d bind=0x%x\n",
                                nc6, obj, w, h, fmt, win32_mem_read32(d + 8),
                                win32_mem_read32(d + 40), pInit ? 1 : 0,
                                win32_mem_read32(d + 36));
                }
            }
            if (g_window_gfx && d && pInit && w && h && w <= 4096 && h <= 4096) {
                uint32_t arr  = win32_mem_read32(d + 12);
                uint32_t misc = win32_mem_read32(d + 40);
                // ✅ ON BY DEFAULT since 2026-08-26. It was gated because the
                // diffuse term this probe is added to comes from the baked-
                // lighting VOLUME, which was empty — so every surface reflected
                // the sky cube at full strength and the room looked like a
                // mirror (scene mean 64 -> 145, visibly worse). **That condition
                // is now satisfied**: the volume carries real data, measured on
                // afghanistan as
                //     [gfx] volume CopyResource 512x256x4 min=0 max=255 mean=2.0
                // and with cubes on the scene is a proper desert vista with depth
                // and atmosphere at 48 fps, not a mirror. Wrong-type texture
                // substitutions fall 2,229,266 -> 165,476 (-93%).
                //
                // The gate's own comment said "this turns on with the volume once
                // that works" — this is that moment. BO2_CUBE=0 turns it back off.
                static const int cube_on = [] {
                    const char* e = bo2_getenv("BO2_CUBE");
                    return (e && e[0] == '0') ? 0 : 1;
                }();
                if (cube_on && arr == 6 && (misc & 0x4u)) {
                    // THE WHOLE MIP CHAIN, not just level 0. D3D lays a cube's
                    // subresources out face-major — index = face*MipLevels + mip —
                    // and the material shader samples the probe at an EXPLICIT
                    // level chosen by surface roughness. Level 0 alone is a mirror,
                    // and these are BC-compressed so Metal cannot generate the rest.
                    uint32_t mips = win32_mem_read32(d + 8);
                    // P773 : MipLevels=0 = chaîne COMPLÈTE sous D3D11, pas 1. Lu comme 1, les
                    // 6 « faces » étaient les 6 premiers niveaux de la face 0 (32 256 octets =
                    // pas 512..16 × 32 lignes) : flashs rose/vert sur les reflets.
                    if (!mips) { mips = 1; for (uint32_t s = w > h ? w : h; s > 1; s >>= 1) mips++; }
                    if (mips > 16) mips = 16;
                    std::vector<uint8_t> cb; std::vector<uint32_t> offs, pits;
                    offs.resize(mips * 6); pits.resize(mips * 6);
                    bool ok = true;
                    for (uint32_t f = 0; f < 6 && ok; f++)
                        for (uint32_t m = 0; m < mips && ok; m++) {
                            uint32_t si = f * mips + m;
                            uint32_t va = win32_mem_read32(pInit + si * 12 + 0);
                            uint32_t rp = win32_mem_read32(pInit + si * 12 + 4);
                            uint32_t lh = h >> m; if (!lh) lh = 1;
                            uint32_t rows = (fmt >= 70 && fmt <= 84) ? (lh + 3) / 4 : lh;
                            uint64_t nb = (uint64_t)rp * rows;
                            if (!va || !rp || !nb || cb.size() + nb > (48u << 20)) {
                                ok = false; break;
                            }
                            offs[si] = (uint32_t)cb.size();
                            pits[si] = rp;
                            cb.resize(cb.size() + (size_t)nb);
                            win32_mem_read_block(cb.data() + offs[si], va, (uint32_t)nb);
                        }
                    if (ok && !cb.empty()) {
                        gfx_register_cube_mips(obj, w, fmt, mips, cb.data(),
                                               (uint32_t)cb.size(), offs.data(),
                                               pits.data());
                        static uint32_t nc = 0;
                        if (++nc <= 8)
                            fprintf(stderr, "[gfx] CUBE obj=%08x %ux%u fmt=%u mips=%u "
                                            "%zu bytes\n", obj, w, h, fmt, mips,
                                    cb.size());
                    }
                }
            }
            // 🎯 THE WHOLE MIP CHAIN FOR PLAIN 2D TEXTURES.
            //
            // Until now only subresource 0 was read, and the Metal side created
            // every 2D texture `mipmapped:NO`. One level means every lookup
            // samples level 0 — and on a ground plane at a grazing angle, which
            // is most of a desert map, minification with no mip chain smears the
            // texture into long streaks that follow the surface and get worse
            // with distance. That is exactly the artefact the tester reported
            // ("les textures parfois bavent") and photographed across afghanistan.
            //
            // The data is already there: D3D hands us an ARRAY of
            // D3D11_SUBRESOURCE_DATA, one per mip (12 bytes each: pSysMem,
            // SysMemPitch, SysMemSlicePitch), and for a non-array 2D texture the
            // subresource index IS the mip level. The cube path above has read
            // its chain this way all along; this is the same read for arr <= 1.
            //
            // ⚠️ These are BC-compressed, so Metal CANNOT generate the chain for
            // us — the game's own prefiltered mips are the only source.
            // 🎯 HOW MANY MIP LEVELS DOES THE GUEST ACTUALLY DECLARE?
            // The 2D mip-chain reader below never fired on a whole run while
            // 103,390 textures resolved, so either these textures really are
            // single-level or the chain arrives by another route. Counting is one
            // line and it settles which.
            if (g_window_gfx && d && w && h) {
                static uint64_t miphist[18] = {0};
                uint32_t ml = win32_mem_read32(d + 8);
                if (fmt >= 70 && fmt <= 84) { g_mipc_bc++; if (fmt < 100) g_mipc_bcfmt[fmt]++; } else g_mipc_plain++;
                miphist[ml < 17 ? ml : 17]++;
                // ⚠️ PRINT EARLY, THEN PERIODICALLY. The first version only spoke
                // every 20,000 creations and never said a word in a full run —
                // "no output" then reads as "no textures", which is the opposite
                // of what was happening. Speak at 200 and 2,000 as well.
                static uint64_t nrep = 0;
                ++nrep;
                if (nrep == 200 || nrep == 2000 || (nrep % 20000) == 0) {
                    fprintf(stderr, "[MIP-CENSUS] CreateTexture2D by declared MipLevels:");
                    for (int q = 0; q < 18; q++)
                        if (miphist[q])
                            fprintf(stderr, " %s%d=%llu", q == 17 ? ">=" : "", q,
                                    (unsigned long long)miphist[q]);
                    fprintf(stderr, "   (0 or 1 = no chain given at creation)\n");
                    // 🎯 AND SAY HOW MANY ARE BLOCK-COMPRESSED, because that is
                    // what decides whether host-side mip generation is even
                    // possible. Metal's generateMipmapsForTexture: refuses a
                    // compressed format, so a BC texture would need
                    // decompress → downsample → recompress — a large piece of
                    // work. An UNCOMPRESSED one is a single Metal call. Before
                    // costing that out, know the split.
                    fprintf(stderr, "[MIP-CENSUS] of those, BLOCK-COMPRESSED (DXGI 70..84, "
                                    "Metal cannot generate mips): %llu; uncompressed "
                                    "(one generateMipmapsForTexture: call each): %llu\n",
                            (unsigned long long)g_mipc_bc,
                            (unsigned long long)g_mipc_plain);
                    fprintf(stderr, "[MIP-CENSUS] block formats in use:");
                    for (int q = 70; q < 100; q++)
                        if (g_mipc_bcfmt[q])
                            fprintf(stderr, " DXGI%d=%llu", q, (unsigned long long)g_mipc_bcfmt[q]);
                    fprintf(stderr, "   (70-72 BC1, 73-75 BC2, 76-78 BC3, 79-80 BC4, "
                                    "81-82 BC5, 83-84 BC6H/BC7)\n");
                }
            }
            // 🎯 IS A MIP CHAIN PRESENT EVEN WHEN MipLevels SAYS 1?
            //
            // The census says every 2D texture declares MipLevels 0 or 1, which
            // for a 2012 AAA title is surprising enough to be worth checking
            // rather than believing. If the game really does hand us a chain, the
            // NEXT subresource's pSysMem would sit exactly one level-0 image
            // after this one. That is a arithmetic test on data we already have,
            // and it distinguishes "the game ships one level" from "we are
            // reading the descriptor wrong" — which need completely different
            // fixes.
            if (g_window_gfx && d && pInit && w >= 64 && h >= 64) {
                static uint64_t nchk = 0, contig = 0, notcontig = 0;
                const uint32_t va0 = win32_mem_read32(pInit + 0);
                const uint32_t rp0 = win32_mem_read32(pInit + 4);
                const uint32_t va1 = win32_mem_read32(pInit + 12);
                const uint32_t rows0 = (fmt >= 70 && fmt <= 84) ? (h + 3) / 4 : h;
                const uint64_t lvl0 = (uint64_t)rp0 * rows0;
                if (va0 && rp0 && lvl0) {
                    ++nchk;
                    const bool looks_chained = va1 && (uint64_t)va1 == (uint64_t)va0 + lvl0;
                    if (looks_chained) ++contig; else ++notcontig;
                    if (nchk == 20 || nchk == 200 || (nchk % 2000) == 0)
                        fprintf(stderr, "[MIP-PROBE] %llu textures >=64px checked: %llu have "
                                        "subresource1 exactly one level-0 image after "
                                        "subresource0 (a real chain), %llu do not — %s\n",
                                (unsigned long long)nchk, (unsigned long long)contig,
                                (unsigned long long)notcontig,
                                contig > notcontig
                                    ? "🎯 A CHAIN IS PRESENT and MipLevels is lying/misread"
                                    : "no chain follows level 0: the game really does ship one "
                                      "level per texture");
                }
            }
            bool mips_done = false;
            if (g_window_gfx && d && pInit && w && h && w <= 8192 && h <= 8192) {
                uint32_t arr2  = win32_mem_read32(d + 12);
                uint32_t misc2 = win32_mem_read32(d + 40);
                uint32_t mips  = win32_mem_read32(d + 8);
                // P773 : MipLevels=0 = chaîne complète (1 104 textures sur 2 000) ; sautée, seul le
                // niveau 0 restait : scintillement des textures BC vues de loin.
                if (!mips) { mips = 1; for (uint32_t s = w > h ? w : h; s > 1; s >>= 1) mips++; }
                if (mips > 16) mips = 16;
                if (arr2 <= 1 && !(misc2 & 0x4u) && mips > 1) {
                    std::vector<uint8_t> mb; std::vector<uint32_t> offs(mips), pits(mips);
                    bool ok = true;
                    for (uint32_t m = 0; m < mips && ok; m++) {
                        uint32_t va = win32_mem_read32(pInit + m * 12 + 0);
                        uint32_t rp = win32_mem_read32(pInit + m * 12 + 4);
                        uint32_t lh = h >> m; if (!lh) lh = 1;
                        uint32_t rows = (fmt >= 70 && fmt <= 84) ? (lh + 3) / 4 : lh;
                        uint64_t nb2 = (uint64_t)rp * rows;
                        if (!va || !rp || !nb2 || mb.size() + nb2 > (48u << 20)) {
                            ok = false; break;
                        }
                        offs[m] = (uint32_t)mb.size();
                        pits[m] = rp;
                        mb.resize(mb.size() + (size_t)nb2);
                        win32_mem_read_block(mb.data() + offs[m], va, (uint32_t)nb2);
                    }
                    if (ok && !mb.empty()) {
                        gfx_register_texture_mips(obj, w, h, fmt, mips, mb.data(),
                                                  (uint32_t)mb.size(), offs.data(),
                                                  pits.data());
                        mips_done = true;
                        static uint32_t nm = 0;
                        if (++nm <= 8)
                            fprintf(stderr, "[gfx] MIPS obj=%08x %ux%u fmt=%u levels=%u "
                                            "%zu bytes\n", obj, w, h, fmt, mips, mb.size());
                    } else {
                        static uint32_t nko = 0;   // P773 : une chaîne refusée retombe sur le niveau 0
                        if (++nko <= 8)
                            fprintf(stderr, "[gfx] MIPS-REFUS obj=%08x %ux%u fmt=%u levels=%u "
                                            "lus=%zu octets\n", obj, w, h, fmt, mips, mb.size());
                    }
                }
            }
            // Hand the image to Metal under the SAME object id the draws will
            // name, so a batch can say which texture it samples.
            if (!mips_done && g_window_gfx && dataVA && w && h && w <= 8192 && h <= 8192) {
                uint32_t nb = pitch ? pitch * ((fmt >= 70 && fmt <= 84) ? (h + 3) / 4 : h)
                                    : w * h * 4;
                if (nb && nb <= (32u << 20)) {
                    std::vector<uint8_t> tb(nb);
                    win32_mem_read_block(tb.data(), dataVA, nb);
                    gfx_register_texture(obj, w, h, fmt, tb.data(), nb, pitch);
                }
            }
            if (d && w && h && w <= 16384 && h <= 16384)
                g_tex2d_wh[obj] = { (uint16_t)w, (uint16_t)h, (uint16_t)fmt,
                                    (uint16_t)win32_mem_read32(d + 8) };
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 4); return true;
        }
        // CreateShaderResourceView(pResource, pDesc, ppView) — SRV -> texture map.
        if (g_track && slot == 9) {           // CreateRenderTargetView(pRes,pDesc,ppRTV)
            uint32_t res = arg(1), pp = arg(3);
            uint32_t vt  = win32_mem_read32(d3d_iface_obj(D3D_IF_VIEW));
            uint32_t obj = win32_guest_alloc(64, 4);
            win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            g_rtv_res[obj] = res;
            // D3D11_RENDER_TARGET_VIEW_DESC: Format 0, ViewDimension 4 (TEXTURE2D 4, TEXTURE2DARRAY 5),
            // MipSlice 8 for both. 10-21 : ignoree jusqu'ici, les 4 niveaux de la chaine de flou du multi
            // (960x540 -> 120x67) s'ecrivaient tous dans le niveau 0 : un quart d'image en haut a gauche en visee.
            const uint32_t pv = arg(2), vdim = pv ? win32_mem_read32(pv + 4) : 0;
            const uint32_t mip = (vdim == 4 || vdim == 5) ? win32_mem_read32(pv + 8) : 0;
            if (mip || vdim == 5) {
                static uint32_t nrm = 0;
                if (++nrm <= 16)
                    fprintf(stderr, "[RTV-MIP] vue %08x ressource=%08x dimension=%u mip=%u tranche=%u (#%u)\n",
                            obj, res, vdim, mip, vdim == 5 ? win32_mem_read32(pv + 12) : 0, nrm);
            }
            if (g_window_gfx) gfx_register_rtv(obj, res, mip);
            if (res && res == g_backbuffer_res) {
                g_backbuffer_rtv = obj;
                if (g_window_gfx) gfx_set_backbuffer_rtv(obj);
                fprintf(stderr, "[gfx] backbuffer RTV = %08x\n", obj);
            }
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 4); return true;
        }
        if (g_track && slot == 7) {
            uint32_t res = arg(1), pp = arg(3);
            uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_VIEW));
            uint32_t obj = win32_guest_alloc(64, 4); win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            g_geo_srv[obj] = res;
            // D3D11_SHADER_RESOURCE_VIEW_DESC: Format 0, ViewDimension 4 (TEXTURE2D 4),
            // Texture2D.MostDetailedMip 8, MipLevels 12 (-1 = down to the last level).
            if (const uint32_t pv = arg(2); pv && win32_mem_read32(pv + 4) == 4) {
                const uint32_t m0 = win32_mem_read32(pv + 8), nl = win32_mem_read32(pv + 12);
                const uint32_t pk = (m0 & 0xFF) << 8 | (nl == 0xFFFFFFFFu ? 0 : (nl & 0xFF));
                const auto t = g_tex2d_wh.find(res);
                const uint32_t tm = t != g_tex2d_wh.end() ? t->second.mips : 1;
                if (m0 || (nl != 0xFFFFFFFFu && tm != 1 && nl != tm)) {   // tm 0 = full chain
                    g_srv_mip[obj] = pk;
                    static uint32_t nsm = 0;
                    if (++nsm <= 16)
                        fprintf(stderr, "[SRV-MIP] vue %08x ressource=%08x (mips %u) niveaux %u+%d (#%u)\n",
                                obj, res, tm, m0, (int)nl, nsm);
                }
            }
            dref_view(obj, res);
            dynref_view(obj, res, slot, win32_mem_read32(esp));
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 4); return true;
        }
        // ⚠️ THE CATCH-ALL USED TO HAND BACK A SINGLETON, and one of the views it
        // covers is CreateDepthStencilView (slot 10) — so EVERY depth view in the
        // game was the same object. Measured: exactly one texture in a whole run
        // is bound and never has pixels, ~2 binds a frame, 6400 over a run, and
        // it is this singleton (0x180001c0 = d3d_iface_obj(D3D_IF_VIEW),
        // allocated from sub_6bbe80). Same family as the ID3D11View::GetResource
        // singleton and the CreateTexture1D/3D singleton before it: a shared
        // object cannot carry per-instance state, so whatever binds it resolves
        // to nothing.
        // Every Create*View here has the same shape — (this, pResource, pDesc,
        // ppView) — so give each one its own object and remember its resource,
        // exactly as slots 7 (SRV) and 9 (RTV) already do.
        uint32_t pp = arg(3);
        if (slot > 6) {
            uint32_t res = arg(1);
            uint32_t vt  = win32_mem_read32(d3d_iface_obj(D3D_IF_VIEW));
            uint32_t obj = win32_guest_alloc(64, 4);
            win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            if (res) g_geo_srv[obj] = res;
            if (slot == 10 && g_depth_dsv) {   // D3D11_DEPTH_STENCIL_VIEW_DESC: Format 0, ViewDimension 4, Flags 8, MipSlice 12, FirstArraySlice 16, ArraySize 20
                const uint32_t pv = arg(2);
                fprintf(stderr, "[DEPTH-DSV] vue %08x ressource=%08x desc=%s format=%u dimension=%u drapeaux=%u mip=%u tranche=%u+%u\n",
                        obj, res, pv ? "lu" : "NUL", pv ? win32_mem_read32(pv) : 0, pv ? win32_mem_read32(pv + 4) : 0,
                        pv ? win32_mem_read32(pv + 8) : 0, pv ? win32_mem_read32(pv + 12) : 0,
                        pv ? win32_mem_read32(pv + 16) : 0, pv ? win32_mem_read32(pv + 20) : 0);
            }
            dref_view(obj, res);
            dynref_view(obj, res, slot, win32_mem_read32(esp));
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 4); return true;
        }
        if (pp) win32_mem_write32(pp, new_res());
        ret(0, 4); return true;
    }
    // IDXGISwapChain::SetFullscreenState(this,Fullscreen,pTarget) slot 10.
    if (iface == D3D_IF_SWAPCHAIN && slot == 10) { ret(0, 3); return true; }
    // IDXGIOutput::GetGammaControlCapabilities(this,pGammaCaps) slot 13 — no HW
    // gamma table (NumGammaControlPoints=0), zero the caps.
    if (iface == D3D_IF_OUTPUT && slot == 13) {
        uint32_t p = arg(1);
        if (p) { for (int i = 0; i < 16; i++) win32_mem_write32(p + i * 4, 0); }
        ret(0, 2); return true;
    }
    // IDXGIOutput::SetGammaControl(this,pArray) slot 14 — accept, no-op.
    if (iface == D3D_IF_OUTPUT && slot == 14) { ret(0, 2); return true; }
    // ID3D11Device shader/state creation, slots 11..26. These fire from the
    // FASTFILE ASSET WALKER (material techset loads create vertex/pixel shaders
    // mid-parse via device vtbl +0x30/+0x3c: FUN_006ba020/FUN_0056f5b0), so a
    // wrong arity here leaks guest ESP inside the XFile walk and silently
    // desyncs the zone stream (the patch.ff EOF-overrun bug). Exact stdcall
    // arities from the standard ID3D11Device vtable; out-param = last arg,
    // filled with a fresh non-null COM object.
    if (iface == D3D_IF_DEVICE && slot >= 11 && slot <= 26) {
        // CreateBlendState(pBlendStateDesc, ppBlendState) — device slot 20.
        // D3D11_BLEND_DESC: AlphaToCoverageEnable +0, IndependentBlendEnable +4,
        // then RenderTarget[8], each a 32-byte D3D11_RENDER_TARGET_BLEND_DESC
        // (BlendEnable, SrcBlend, DestBlend, BlendOp, SrcBlendAlpha, DestBlendAlpha,
        // BlendOpAlpha, RenderTargetWriteMask — the last a UINT8 padded to a dword).
        // Only RenderTarget[0] is read: BO2 does not set IndependentBlendEnable.
        if (g_track && slot == 20) {
            uint32_t pd = arg(1), pp = arg(2);
            uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
            uint32_t obj = win32_guest_alloc(64, 4);
            win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            GfxBlend bl = kBlendDefault;
            if (pd) {
                uint32_t rt = pd + 8;
                bl.enable     = win32_mem_read32(rt + 0);
                bl.src_rgb    = win32_mem_read32(rt + 4);
                bl.dst_rgb    = win32_mem_read32(rt + 8);
                bl.op_rgb     = win32_mem_read32(rt + 12);
                bl.src_a      = win32_mem_read32(rt + 16);
                bl.dst_a      = win32_mem_read32(rt + 20);
                bl.op_a       = win32_mem_read32(rt + 24);
                bl.write_mask = win32_mem_read32(rt + 28) & 0xFu;
            }
            g_blend_states[obj] = bl;
            static uint32_t nbs = 0;
            if (++nbs <= 12)
                fprintf(stderr, "[D3D-BLEND] state %08x: enable=%u rgb(%u,%u,op %u) "
                                "a(%u,%u,op %u) mask=0x%x\n",
                        obj, bl.enable, bl.src_rgb, bl.dst_rgb, bl.op_rgb,
                        bl.src_a, bl.dst_a, bl.op_a, bl.write_mask);
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 3); return true;
        }
        // CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState) — device
        // slot 21, immediately after CreateBlendState. D3D11_DEPTH_STENCIL_DESC:
        // BOOL DepthEnable +0, D3D11_DEPTH_WRITE_MASK DepthWriteMask +4,
        // D3D11_COMPARISON_FUNC DepthFunc +8, BOOL StencilEnable +12, UINT8 StencilReadMask +16,
        // UINT8 StencilWriteMask +17, then FrontFace +20 and BackFace +36, each {StencilFailOp,
        // StencilDepthFailOp, StencilPassOp, StencilFunc} (layout re-read in the game's one factory, t6sp 0x6f81f0).
        if (g_track && slot == 21) {
            uint32_t pd = arg(1), pp = arg(2);
            uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
            uint32_t obj = win32_guest_alloc(64, 4);
            win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            GfxDepth dz = kDepthDefault;
            uint32_t st_jeu = 0;                 // StencilEnable as the game asks it, whatever BO2_STENCIL says
            if (pd) {
                dz.enable = win32_mem_read32(pd + 0);
                dz.write  = win32_mem_read32(pd + 4);
                dz.func   = win32_mem_read32(pd + 8);
                st_jeu = win32_mem_read32(pd + 12) ? 1 : 0;
                if (g_pochoir && st_jeu) {
                    const uint32_t m = win32_mem_read32(pd + 16);
                    dz.st = 1u | ((m & 0xFFu) << 8) | (((m >> 8) & 0xFFu) << 16);
                    for (int i = 0; i < 8; i++)
                        dz.st_ops |= (win32_mem_read32(pd + 20 + 4 * i) & 0xFu) << (4 * i);
                }
            }
            g_depth_states[obj] = dz;
            static uint32_t nds = 0, nst = 0, nst_jeu = 0;
            nst_jeu += st_jeu;
            if (++nds <= 12)
                fprintf(stderr, "[D3D-DEPTH] state %08x: enable=%u write=%u func=%u\n",
                        obj, dz.enable, dz.write, dz.func);
            if (dz.st && ++nst <= 64)
                fprintf(stderr, "[POCHOIR] etat %08x : lecture=%02x ecriture=%02x avant F/Z/P/f=%u/%u/%u/%u "
                                "arriere=%u/%u/%u/%u profondeur=%u/%u/%u\n", obj, (dz.st >> 8) & 0xFF, dz.st >> 16,
                        dz.st_ops & 15, (dz.st_ops >> 4) & 15, (dz.st_ops >> 8) & 15, (dz.st_ops >> 12) & 15,
                        (dz.st_ops >> 16) & 15, (dz.st_ops >> 20) & 15, (dz.st_ops >> 24) & 15, dz.st_ops >> 28,
                        dz.enable, dz.write, dz.func);
            if ((nds & (nds - 1)) == 0)
                fprintf(stderr, "[POCHOIR] etats de profondeur crees=%u dont StencilEnable=1 : %u (traduits : %u)\n",
                        nds, nst_jeu, nst);
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 3); return true;
        }
        // CreateRasterizerState(pRasterizerDesc, ppRasterizerState) — device slot 22. It used to fall into the
        // generic path, which hands every caller the SAME shared object: RSSetState could not tell two apart.
        if (g_track && slot == 22) {
            uint32_t pd = arg(1), pp = arg(2);
            uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
            uint32_t obj = win32_guest_alloc(64, 4);
            win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            EtatRaster er = kRasterDefaut;
            static uint32_t nrs = 0, n_cull[4], n_ciseau, n_ccw;
            if (pd) {
                er.cull = (uint8_t)win32_mem_read32(pd + 4); er.ciseau = win32_mem_read32(pd + 28) ? 1 : 0;
                // FrontCounterClockwise: front counter-clockwise + BACK is front clockwise (Metal's default) + FRONT.
                if (win32_mem_read32(pd + 8)) { ++n_ccw; if (er.cull >= 2) er.cull ^= 1; }
                // DepthBias +12, DepthBiasClamp +16, SlopeScaledDepthBias +20, DepthClipEnable +24.
                const uint32_t bm = win32_mem_read32(pd + 16), bp = win32_mem_read32(pd + 20);
                er.biais = (int32_t)win32_mem_read32(pd + 12); memcpy(&er.biais_max, &bm, 4); memcpy(&er.pente, &bp, 4);
                er.pince_z = win32_mem_read32(pd + 24) ? 0 : 1;
                static std::set<std::array<uint32_t, 4>> vus;   // one line per distinct combination
                if ((er.biais || bp || er.pince_z) && vus.insert({(uint32_t)er.biais, bm, bp, er.pince_z}).second)
                    fprintf(stderr, "[DEPTH-BIAS] etat de rasterisation : DepthBias=%d pente=%g max=%g DepthClipEnable=%u (%s)\n",
                            er.biais, er.pente, er.biais_max, !er.pince_z, g_depth_bias ? "traduits" : "NON traduits");
            }
            g_raster_states[obj] = er;
            ++n_cull[er.cull & 3]; n_ciseau += er.ciseau;
            if (puiss2(++nrs))
                fprintf(stderr, "[CISEAU] etats de rasterisation crees=%u dont ScissorEnable=1 : %u ; CullMode "
                                "NONE/FRONT/BACK = %u/%u/%u (apres FrontCounterClockwise=1 : %u)\n", nrs, n_ciseau,
                        n_cull[1], n_cull[2], n_cull[3], n_ccw);
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 3); return true;
        }
        // P805 : CreateSamplerState(pSamplerDesc, ppSamplerState) — device slot 23. The game's own
        // address modes (WRAP for tiled ground), filters and comparison went nowhere: every draw sampled
        // through one sampler whose address mode is Metal's default CLAMP_TO_EDGE, so any UV past 1
        // smeared the texture's edge row across the surface — the square streaked patches on angola.
        // D3D11_SAMPLER_DESC is 13 dwords: Filter, AddressU/V/W, MipLODBias, MaxAnisotropy,
        // ComparisonFunc, BorderColor[4], MinLOD, MaxLOD.
        if (g_track && slot == 23) {
            uint32_t pd = arg(1), pp = arg(2);
            uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
            uint32_t obj = win32_guest_alloc(64, 4);
            win32_mem_write32(obj + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
            if (pd && g_window_gfx) {
                uint32_t desc[13];
                for (int i = 0; i < 13; i++) desc[i] = win32_mem_read32(pd + i * 4);
                gfx_register_sampler(obj, desc);
                static uint32_t nss = 0;
                if (++nss <= 12)
                    fprintf(stderr, "[D3D-SAMPLER] state %08x: filter=0x%x addr=%u,%u,%u aniso=%u cmp=%u\n",
                            obj, desc[0], desc[1], desc[2], desc[3], desc[5], desc[6]);
            }
            if (pp) win32_mem_write32(pp, obj);
            ret(0, 3); return true;
        }
        uint32_t nargs;
        switch (slot) {
            case 11: nargs = 6;  break;   // CreateInputLayout
            case 12: case 13: case 15:
            case 16: case 17: case 18:
                     nargs = 5;  break;   // CreateVS/GS/PS/Hull/Domain/CS
            case 14: nargs = 10; break;   // CreateGeometryShaderWithStreamOutput
            case 19: nargs = 2;  break;   // CreateClassLinkage
            default: nargs = 3;  break;   // 20-26 CreateBlend/DepthStencil/
                                          // Rasterizer/Sampler/Query/Predicate/Counter
        }
        // Render-workload characterization: shader creates carry the compiled
        // DXBC bytecode (arg1 = ptr, arg2 = length) — capturing ptr/size/fourcc
        // sizes the DXBC->MSL translation job for a future Metal backend.
        if (slot >= 12 && slot <= 18 && slot != 14) {
            static std::unordered_map<int,int> sh_seen;
            uint32_t bc = arg(1), len = arg(2);
            const char* kind = slot==12?"VS":slot==13?"GS":slot==15?"PS":
                               slot==16?"HS":slot==17?"DS":"CS";
            if (sh_seen[slot]++ < 4)
                fprintf(stderr, "[D3D-SHADER] %s bc=0x%08x len=%u fourcc=0x%08x\n",
                        kind, bc, len, bc ? win32_mem_read32(bc) : 0);
            // BO2_SHADER_DUMP=1: write each DISTINCT shader's DXBC to
            // shader_<kind>_<hash>.dxbc. How many the frontend actually needs is
            // the decision point between hand-translating them to MSL and
            // bridging a full DXBC translator (DXVK/DXMT) — so count them first.
            static const int dump = bo2_getenv("BO2_SHADER_DUMP") ? 1 : 0;
            // Translate whenever the window is up: the Metal side needs the MSL
            // keyed by the same object the draws will bind.
            if ((dump || g_window_gfx) && bc && len && len < (4u << 20)) {
                g_sh_pending_len = len;
                // KEEP the bytecode; translate on first BIND (below). Translating
                // all 2518 shaders here blocked the guest thread long enough for
                // the stall watchdog to fire ("no import calls for ~24 s") — the
                // engine creates the whole frontend set up front but the menu only
                // ever binds ~314 of them.
                if (g_window_gfx && (slot == 12 || slot == 15)) {
                    g_sh_bc_pending.resize(len);
                    win32_mem_read_block(g_sh_bc_pending.data(), bc, len);
                }
            }
            if (dump && bc && len && len < (4u << 20)) {
                static std::unordered_map<uint64_t,int> seen;
                std::vector<uint8_t> b(len);
                win32_mem_read_block(b.data(), bc, len);
                uint64_t h = 1469598103934665603ull;
                for (uint32_t i = 0; i < len; i++) h = (h ^ b[i]) * 1099511628211ull;
                if (!seen[h]++) {
                    char path[64];
                    snprintf(path, sizeof path, "shader_%s_%016llx.dxbc", kind,
                             (unsigned long long)h);
                    FILE* f = fopen(path, "wb");
                    if (f) { fwrite(b.data(), 1, len, f); fclose(f); }
                    fprintf(stderr, "[SHADER-DUMP] %s %u bytes -> %s (distinct so far %zu)\n",
                            kind, len, path, seen.size());
                }
            }
        }
        if (slot == 11) {                      // CreateInputLayout(pDescs,n,vsBc,vsLen,ppLayout)
            static int il = 0;
            if (il++ < 4)
                fprintf(stderr, "[D3D-INPUTLAYOUT] pDescs=0x%08x n=%u vsBc=0x%08x\n",
                        arg(1), arg(2), arg(3));
            if (g_window_gfx) {
                // D3D11_INPUT_ELEMENT_DESC is SEVEN dwords = 28 BYTES, not 32:
                //   LPCSTR SemanticName; UINT SemanticIndex; DXGI_FORMAT Format;
                //   UINT InputSlot; UINT AlignedByteOffset;
                //   D3D11_INPUT_CLASSIFICATION InputSlotClass;
                //   UINT InstanceDataStepRate;
                // Striding by 32 read element 0 correctly and turned every later
                // one into garbage, so only POSITION0 ever matched and Metal
                // refused the pipeline with "Vertex attribute COLOR0(1) is missing
                // from the vertex descriptor". The per-element log printed the
                // bogus semantic as literally "28" — the real stride.
                uint32_t pd = arg(1), n = arg(2), pp = arg(5);
                std::vector<GfxDrawElem> els;
                for (uint32_t i = 0; i < n && i < 32; i++) {
                    uint32_t e = pd + i * 28;
                    uint32_t namep = win32_mem_read32(e + 0);
                    GfxDrawElem el{};
                    // HLSLcc concatenates semantic name and index (POSITION0), so
                    // build the same key here.
                    char nm[20] = {0};
                    for (int c = 0; c < 19 && namep; c++) {
                        uint8_t ch = win32_mem_read8(namep + (uint32_t)c);
                        if (!ch) break;
                        nm[c] = (char)ch;
                    }
                    snprintf(el.semantic, sizeof el.semantic, "%s%u", nm,
                             win32_mem_read32(e + 4));
                    el.fmt    = win32_mem_read32(e + 8);
                    el.slot   = win32_mem_read32(e + 12);
                    el.offset = win32_mem_read32(e + 16);
                    els.push_back(el);
                }
                // CACHE by descriptor contents. The engine calls CreateInputLayout
                // thousands of times with the same layouts; allocating a guest
                // object per call leaked the guest heap until the FASTFILE STREAMING
                // ring could no longer be served and its watchdog aborted the run at
                // frame ~700 — a failure that looked like a graphics bug because it
                // only appeared with BO2_WINDOW=1.
                uint64_t h = 1469598103934665603ull;
                for (const GfxDrawElem& e : els) {
                    for (const char* c = e.semantic; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
                    for (uint32_t v : { e.fmt, e.offset, e.slot }) h = (h ^ v) * 1099511628211ull;
                }
                static std::unordered_map<uint64_t, uint32_t> il_cache;
                uint32_t obj = 0;
                auto c = il_cache.find(h);
                if (c != il_cache.end()) {
                    obj = c->second;
                } else {
                    uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
                    obj = win32_guest_alloc(64, 4);
                    win32_mem_write32(obj + 0, vt);
                    for (int i = 4; i < 64; i += 4) win32_mem_write32(obj + i, 0);
                    g_il[obj] = std::move(els);
                    il_cache[h] = obj;
                }
                if (pp) win32_mem_write32(pp, obj);
                ret(0, nargs); return true;
            }
        }
        uint32_t pp = arg(nargs - 1);
        // new_res() is a SINGLETON: every resource the shim hands back is the same
        // fake object. That is why the created-shader count read 1 — the shim has
        // no per-object identity at all, which is itself a finding: a real backend
        // bridge needs one object per D3D resource. Under BO2_SHADER_DUMP give each
        // shader its own object so the number the menu actually BINDS is
        // measurable; the default path keeps the singleton.
        uint32_t nobj = new_res();
        if (slot >= 12 && slot <= 18 && slot != 14 && g_sh_pending_len) {
            uint32_t vt = win32_mem_read32(d3d_iface_obj(D3D_IF_RESOURCE));
            uint32_t o  = win32_guest_alloc(64, 4);
            win32_mem_write32(o + 0, vt);
            for (int i = 4; i < 64; i += 4) win32_mem_write32(o + i, 0);
            nobj = o;
            g_sh_obj[nobj] = (slot == 15) ? 'P' : (slot == 12) ? 'V' : 'O';
            g_sh_pending_len = 0;
            if (g_window_gfx && !g_sh_bc_pending.empty() && (slot == 12 || slot == 15)) {
                g_sh_bc[nobj] = std::move(g_sh_bc_pending);
                g_sh_bc_pending.clear();
                static const int cree = bo2_env_on("BO2_SHADER_CREE") ? 1 : 0;
                if (cree) {
                    static uint32_t ncree = 0;
                    translate_shader(nobj, slot == 15 ? 9 : 11);
                    if (++ncree <= 3 || ncree % 500 == 0)
                        fprintf(stderr, "[MSL-CREE] %u shaders traduits a la creation\n", ncree);
                }
            }
        }
        if (pp) win32_mem_write32(pp, nobj);
        ret(0, nargs); return true;
    }
    // ID3D11Device::CheckFormatSupport(this,Format,pSupport) slot 27 — say "all".
    if (iface == D3D_IF_DEVICE && slot == 27) {
        uint32_t ps = arg(2);
        if (ps) win32_mem_write32(ps, 0xFFFFFFFFu);
        ret(0, 3); return true;
    }
    // ID3D11DeviceContext — FULL arity table (slots 3..114 of the standard
    // vtable). Every context method must clean its exact stdcall arity: the
    // renderer maps/unmaps dynamic buffers in tight per-buffer init loops
    // (FUN_00694d50 -> FUN_006e5880), and the clean1 fallback leaked
    // 20B (Map) + 8B (Unmap) = 28B of guest ESP per iteration until the
    // RUNAWAY watchdog fired. Set*/Draw*/Copy*/Clear* are accepted as no-ops,
    // Get* return "nothing bound", Map hands out a shared writable scratch
    // block (no GPU backing yet — the game only needs somewhere to write).
    if (iface == D3D_IF_CONTEXT && slot >= 3 && slot <= 114) {
        static const uint8_t ctx_args[112] = {
            2,4,4,3,4,4,4,4,   // 3-10  GetDevice..PSSetSamplers
            4,4,3,6,3,4,2,6,   // 11-18 VSSetShader,DrawIndexed,Draw,Map,Unmap,
                               //       PSSetCBufs,IASetInputLayout,IASetVBufs
            4,6,5,4,4,2,4,4,   // 19-26 IASetIB,DrawIdxInst,DrawInst,GSSetCBufs,
                               //       GSSetShader,IASetTopology,VSSetSRVs,VSSetSamplers
            2,2,5,3,4,4,4,8,   // 27-34 Begin,End,GetData,SetPredication,GSSetSRVs,
                               //       GSSetSamplers,OMSetRTs,OMSetRTsAndUAVs
            4,3,4,1,3,3,4,3,   // 35-42 OMSetBlend,OMSetDS,SOSetTargets,DrawAuto,
                               //       DrawIdxInstIndirect,DrawInstIndirect,Dispatch,DispatchIndirect
            2,3,3,9,3,7,4,3,   // 43-50 RSSetState,RSSetViewports,RSSetScissors,
                               //       CopySubres,CopyRes,UpdateSubres,CopyStructCount,ClearRTV
            3,3,5,2,3,2,6,3,   // 51-58 ClearUAVUint,ClearUAVFloat,ClearDSV,GenMips,
                               //       SetResMinLOD,GetResMinLOD,ResolveSubres,ExecCmdList
            4,4,4,4,4,4,4,4,   // 59-66 HS/DS Set{SRVs,Shader,Samplers,CBufs}
            4,5,4,4,4,4,4,4,   // 67-74 CSSetSRVs,CSSetUAVs,CSSetShader,CSSetSamplers,
                               //       CSSetCBufs,VSGetCBufs,PSGetSRVs,PSGetShader
            4,4,4,2,6,4,4,4,   // 75-82 PSGetSamplers,VSGetShader,PSGetCBufs,
                               //       IAGetInputLayout,IAGetVBufs,IAGetIB,GSGetCBufs,GSGetShader
            2,4,4,3,4,4,4,7,   // 83-90 IAGetTopology,VSGetSRVs,VSGetSamplers,GetPredication,
                               //       GSGetSRVs,GSGetSamplers,OMGetRTs,OMGetRTsAndUAVs
            4,3,3,2,3,3,4,4,   // 91-98 OMGetBlend,OMGetDS,SOGetTargets,RSGetState,
                               //       RSGetViewports,RSGetScissors,HSGetSRVs,HSGetShader
            4,4,4,4,4,4,4,4,   // 99-106 HSGetSamplers,HSGetCBufs,DSGet{SRVs,Shader,
                               //       Samplers,CBufs},CSGetSRVs,CSGetUAVs
            4,4,4,1,1,1,1,3 }; // 107-114 CSGetShader,CSGetSamplers,CSGetCBufs,
                               //       ClearState,Flush,GetType,GetContextFlags,FinishCmdList
        int nargs = (int)ctx_args[slot - 3];
        // BO2_D3D_SITES=1: log each DISTINCT (slot, guest-ret) once — a complete
        // map of which backend functions touch the D3D context. Used to find the
        // 2D/LUI material pass (or prove it never runs): only ONE DrawIndexed
        // site exists (0x6d379a, the fullscreen-quad helper), so either the UI
        // pass isn't reached or it bails before its draw.
        {
            static bool s_sites = bo2_getenv("BO2_D3D_SITES") != nullptr;
            if (s_sites) {
                static std::unordered_map<uint64_t, bool> seen;
                uint64_t key = ((uint64_t)slot << 32) | win32_mem_read32(esp);
                if (!seen.count(key)) {
                    seen[key] = true;
                    fprintf(stderr, "[D3D-SITE] slot=%d ret=0x%06x tid=%d frame=%u\n",
                            slot, win32_mem_read32(esp), win32_current_tid(), g_present_frames);
                }
            }
        }
        // Draw-workload characterization (the geometry a real backend must
        // execute): DrawIndexed(idxCount,startIdx,baseVtx)=12, Draw(vtxCount,
        // start)=13, DrawIndexedInstanced=20, DrawInstanced=21, DrawAuto=39.
        if (slot==12||slot==13||slot==20||slot==21||slot==39) {
            static uint32_t draws = 0;
            // First 40 in full, then every 1000: the cap alone made the total
            // unreadable (it looked pinned at 40 once draws finally started).
            if (draws < 40 || draws % 1000 == 0)
                // [esp] = guest return address of the vtbl call — identifies WHICH
                // backend function issues this draw (the working example to trace
                // backwards from: only the bg quad is ever drawn, LUI never emits).
                fprintf(stderr, "[D3D-DRAW] slot=%d a1=%u a2=%u a3=%u (#%u) ret=0x%06x tid=%d frame=%u\n",
                        slot, arg(1), arg(2), arg(3), draws,
                        win32_mem_read32(esp), win32_current_tid(), g_present_frames);
            // GEOMETRY CAPTURE: accumulate every DISTINCT quad the menu draws
            // across the whole run (deduped + capped inside geo_dump_draw), so a
            // sparse per-frame UI still builds up into the full menu layout.
            if (g_geo && slot == 12)
                geo_dump_draw(g_present_frames, draws, arg(1), arg(2), arg(3));
            // LIVE path. BO2_UI_FIXED=1 keeps the old fixed-function decode (our
            // own shader, positions transformed by the matrix we read out of VS
            // CB0); by default hand Metal the whole bound state so it can run the
            // game's OWN vertex and pixel shaders.
            if (g_window_gfx && slot == 12) {
                static const int fixed_fn = bo2_getenv("BO2_UI_FIXED") ? 1 : 0;
                if (fixed_fn) ui_collect_draw(arg(1), arg(2), arg(3));
                else {
                    // A stencil MASK dropped by one of real_draw's early exits leaves its children tested
                    // against a plane that was never written: name it.
                    const uint64_t av = g_st_emis;
                    real_draw(arg(1), arg(2), (int32_t)arg(3));
                    if (g_st_emis == av && g_cur_depth.st && g_cur_dsv_res && puiss2(++g_st_rejetes))
                        fprintf(stderr, "[POCHOIR] dessins a pochoir REJETES par real_draw (ps=%08x indices=%u, "
                                        "cumul %llu)\n", g_cur_ps, arg(1), (unsigned long long)g_st_rejetes);
                }
            }
            // Only DrawIndexed reaches Metal. A stencil MASK issued through another slot would leave the plane
            // at 0 and its LESS_EQUAL children would vanish: say so instead of letting it look like a stencil bug.
            else if (g_cur_depth.st && g_cur_dsv_res && puiss2(++g_st_non_exec))
                fprintf(stderr, "[POCHOIR] dessins a pochoir NON EXECUTES (slot %d, cumul %llu)\n",
                        slot, (unsigned long long)g_st_non_exec);
            if (slot != 12 && g_cur_dsv_prof) {   // same blind spot for a depth plane: an atlas drawn through Draw / DrawInstanced stays empty
                static uint64_t n = 0, n_sc = 0;   // n_sc: without a colour target, the only ones an atlas can miss
                const bool sc = !g_cur_rtv && puiss2(++n_sc);
                if (puiss2(++n) || sc) fprintf(stderr, "[DEPTH-DSV] dessins avec DSV NON EXECUTES (slot %d, rtv=%08x res=%08x, cumul %llu dont sans couleur=%llu)\n",
                                               slot, g_cur_rtv, g_cur_dsv_prof, (unsigned long long)n, (unsigned long long)n_sc);
            }
            draws++;
        }
        switch (slot) {
            case 3: { uint32_t pp = arg(1);      // GetDevice
                      if (pp) win32_mem_write32(pp, d3d_iface_obj(D3D_IF_DEVICE));
                      break; }
            case 14: {                           // Map(pRes,Sub,Type,Flags,pMapped)
                static uint32_t map_scratch = 0;
                if (!map_scratch) map_scratch = win32_guest_alloc(16u << 20, 16);
                uint32_t pm = arg(5);
                // GEOMETRY CAPTURE: a tracked buffer maps to ITS OWN backing so
                // VB/IB don't alias; everything else keeps the shared scratch.
                uint32_t data = map_scratch, pitch = 1920 * 4;
                // A DYNAMIC TEXTURE gets its own backing and its REAL row pitch.
                // The shared scratch with a hardcoded 1920*4 pitch was fine for
                // buffers and silently wrong for textures: the guest wrote each
                // decoded video plane into scratch at the wrong stride and nothing
                // ever uploaded it. Handled before the buffer path because a
                // texture is never in g_geo_bufs.
                {
                    auto dt = g_dyntex.find(arg(1));
                    if (dt != g_dyntex.end()) {
                        // ⚠️ POOLING THIS BUFFER ACROSS LIVE TEXTURES CORRUPTS THE
                        // PICTURE — tried, and it put GREEN VERTICAL BANDS on
                        // screen: the guest does not rewrite the whole surface on
                        // every Map, so a backing carries content forward and
                        // handing texture B the buffer texture A just finished
                        // with mixes their planes. So each LIVE texture keeps its
                        // own backing.
                        //
                        // But the game makes a NEW dynamic texture per video frame
                        // and abandons the old one (AddRef/Release cannot tell us
                        // when — see the note there), so without a bound the arena
                        // fills and the GAME reports our bug as
                        // "Out of memory error" and exits. EVICT BY AGE INSTEAD OF
                        // REUSING BY SIZE: a texture still in play is Mapped every
                        // frame, so anything untouched for many ticks is dead and
                        // its backing can go back. That frees the memory without
                        // ever recycling a buffer under a live surface — which is
                        // exactly the distinction the pooling attempt missed.
                        dt->second.used = ++g_dyntex_tick;
                        if (!dt->second.backing) {
                            if (g_dyntex_live > 24) {
                                for (auto& kv : g_dyntex) {
                                    if (!kv.second.backing) continue;
                                    if (g_dyntex_tick - kv.second.used < 300) continue;
                                    win32_guest_free(kv.second.backing);
                                    kv.second.backing = 0;
                                    g_dyntex_live--; g_dyntex_evicted++;
                                }
                            }
                            dt->second.backing = win32_guest_alloc(dt->second.bytes, 16);
                            if (dt->second.backing) g_dyntex_live++;
                        }
                        if (dt->second.backing) {
                            if (pm) {
                                // D3D11_MAPPED_SUBRESOURCE = { pData, RowPitch,
                                // DepthPitch }. For a 2D texture DepthPitch is the
                                // size of the whole slice, which IS `bytes` — but
                                // for a VOLUME it is the size of ONE z-slice, and
                                // `bytes` is that times the depth.
                                //
                                // 🪤 WE HANDED VOLUMES `bytes`. The room's baked
                                // lighting is 512x256x4 RGBA8: RowPitch 2048,
                                // DepthPitch 524288, total 2097152. So the guest
                                // was told each z-slice is 2 MB apart when they are
                                // 512 KB apart, and `pData + z*DepthPitch` put
                                // slice 1 exactly at the END of the 2 MB buffer and
                                // slices 2-3 well past it. Only slice 0 could ever
                                // land — which is why the volume reads back with a
                                // mean of about 1/255 while its max is 255, and why
                                // the room has no diffuse term.
                                const uint32_t depth_pitch =
                                    (dt->second.depth > 1 && dt->second.slice)
                                        ? dt->second.slice : dt->second.bytes;
                                win32_mem_write32(pm + 0, dt->second.backing);
                                win32_mem_write32(pm + 4, dt->second.pitch);
                                win32_mem_write32(pm + 8, depth_pitch);
                            }
                            break;
                        }
                    }
                }
                if (g_track) {
                    g_buf_gen[arg(1)]++;
                    auto b = g_geo_bufs.find(arg(1));
                    if (b != g_geo_bufs.end()) {
                        // Sous Windows un tampon dynamique garde son contenu jusqu'a son Map suivant, et
                        // NO_OVERWRITE ajoute sans toucher a ce qui y est deja. La tranche d'anneau, elle,
                        // est rendue a chaque Present, et l'adresse de la suivante depend de l'ordre des
                        // Map de l'image. (1) Constantes : un dessin d'une image sans Map partait sans elles
                        // (perdus_cb ~1 550 par image sur angola). (2) Sommets : un tampon ecrit UNE fois puis
                        // redessine changeait de tranche avec l'ordre des Map. (Les decalques qui clignotaient,
                        // crateres et sang, venaient du biais de profondeur : BO2_DEPTH_BIAS, pas d'ici.)
                        // Stockage propre, garde d'un Map a l'autre ; les dessins recopient a chaque dessin
                        // (sommets) ou a chaque Map (constantes, g_buf_gen).
                        bool cb_neuf = false;
                        if (!b->second.backing && b->second.size && b->second.size <= (16u << 20)) {
                            b->second.backing = win32_guest_alloc((b->second.size + 15u) & ~15u, 16);
                            static uint64_t n = 0, octets = 0;
                            if (b->second.backing) {
                                cb_neuf = true; ++n; octets += b->second.size;
                                if (n <= 2 || (n & (n - 1)) == 0)
                                    fprintf(stderr, "[DYN-PERSISTANT] tampon dynamique #%llu (bind=%u, %u o) garde son "
                                                    "contenu entre deux Map (%llu ko au total)\n",
                                            (unsigned long long)n, b->second.bind, b->second.size,
                                            (unsigned long long)(octets >> 10));
                            }
                        }
                        if (b->second.backing) {          // static, created with data
                            if (g_buf_gen[arg(1)] == 1 && !cb_neuf) ++g_c50_statiques_mappes;   // C50 : plus resident
                            data = b->second.backing; pitch = b->second.size;
                        } else if (b->second.size) {
                            // Dynamic: bump-allocate out of a fixed ring, so this
                            // Map cannot land on the bytes another live buffer is
                            // still using. Reset every Present, which is well
                            // after the draws that read it.
                            static uint32_t ring = 0, ring_sz = 32u << 20;
                            if (!ring) ring = win32_guest_alloc(ring_sz, 16);
                            uint32_t need = (b->second.size + 15u) & ~15u;
                            if (ring && need < ring_sz) {
                                // ONE slice per buffer per frame. Handing out a
                                // fresh slice on every Map loses everything an
                                // earlier Map of the same buffer wrote, which is
                                // exactly how D3D11 dynamic buffers are used
                                // (map, append, unmap, draw, repeat) — the draws
                                // then read unwritten bytes and the menu came out
                                // as long gradient fans.
                                auto prev = g_ui_ring_map.find(arg(1));
                                if (prev != g_ui_ring_map.end()) {
                                    data  = prev->second;
                                    pitch = b->second.size;
                                } else {
                                    if (g_ui_ring_off + need > ring_sz) g_ui_ring_off = 0;
                                    data  = ring + g_ui_ring_off;
                                    pitch = b->second.size;
                                    g_ui_ring_map[arg(1)] = data;
                                    g_ui_ring_off += need;
                                }
                            }
                        }
                    }
                }
                if (pm) {
                    win32_mem_write32(pm + 0, data);               // pData
                    win32_mem_write32(pm + 4, pitch);              // RowPitch
                    win32_mem_write32(pm + 8, pitch * 1080);       // DepthPitch
                }
                break; }
            case 15: {                           // Unmap(pRes, Subresource)
                // THE DELIVERY STEP. Map handed the guest a backing buffer; this
                // is where what it wrote actually becomes a texture. Without it
                // every decoded video frame was written and discarded.
                auto dt = g_dyntex.find(arg(1));
                if (g_window_gfx && dt != g_dyntex.end() && dt->second.backing) {
                    static std::vector<uint8_t> tb;
                    tb.resize(dt->second.bytes);
                    win32_mem_read_block(tb.data(), dt->second.backing, dt->second.bytes);
                    if (dt->second.depth > 1)
                        gfx_register_texture_vol(arg(1), dt->second.w, dt->second.h,
                                                 dt->second.depth, 1, dt->second.fmt,
                                                 tb.data(), dt->second.bytes,
                                                 dt->second.pitch, dt->second.slice);
                    else
                        gfx_register_texture(arg(1), dt->second.w, dt->second.h,
                                             dt->second.fmt, tb.data(),
                                             dt->second.bytes, dt->second.pitch);
                    // IS THE FRAME WE ARE UPLOADING ACTUALLY BLACK? The user
                    // reports brief full-screen black flashes, and there are two
                    // very different explanations — a staging buffer we recycled
                    // out from under a live texture, or a genuinely black frame
                    // from the game. Counting black uploads separates them, and
                    // guessing has already cost three reverts today.
                    {
                        g_dyntex_up++;
                        bool black = true;
                        for (uint32_t q = 0; q < dt->second.bytes && black; q += 997)
                            if (tb[q] > 8) black = false;
                        if (black) g_dyntex_black++;
                        if (bo2_getenv("BO2_DYNTEX") && (g_dyntex_up % 200) == 0)
                            fprintf(stderr, "[DYNTEX] uploads=%llu black=%llu "
                                            "live=%llu evicted=%llu\n",
                                    (unsigned long long)g_dyntex_up,
                                    (unsigned long long)g_dyntex_black,
                                    (unsigned long long)g_dyntex_live,
                                    (unsigned long long)g_dyntex_evicted);
                    }
                    static uint32_t nup = 0;
                    if (bo2_getenv("BO2_DYNTEX") && (++nup <= 12 || (nup % 500) == 0))
                        fprintf(stderr, "[DYNTEX] upload #%u obj=%08x %ux%u fmt=%u "
                                        "pitch=%u first=%02x %02x %02x %02x\n",
                                nup, arg(1), dt->second.w, dt->second.h, dt->second.fmt,
                                dt->second.pitch, tb[0], tb[1], tb[2], tb[3]);
                }
                break; }
            case 44: if (g_track) {              // RSSetViewports(Num,pViewports)
                        uint32_t n = arg(1), p = arg(2);
                        if (p) for (uint32_t i=0;i<n&&i<8;i++) { uint32_t vp = p + i*24;
                            if (i == 0) {        // the UI pass renders into vp[0]
                                float w = geo_f32(vp+8), h = geo_f32(vp+12);
                                if (w > 16.f && h > 16.f) { g_ui_vpw = w; g_ui_vph = h; }
                                // Remember it verbatim too: a pass into the 480x270
                                // downsample target needs ITS viewport, not the
                                // main one.
                                g_vp_now[0] = geo_f32(vp+0); g_vp_now[1] = geo_f32(vp+4);
                                g_vp_now[2] = w;             g_vp_now[3] = h;
                                // +16/+20 = MinDepth/MaxDepth, les deux champs
                                // que ce shim n'avait jamais lus.
                                g_vp_now[4] = geo_f32(vp+16);
                                g_vp_now[5] = geo_f32(vp+20);
                            }
                            if (g_geo) geo_state("V %.1f %.1f %.1f %.1f\n",
                                geo_f32(vp), geo_f32(vp+4), geo_f32(vp+8), geo_f32(vp+12)); }
                     } break;
            case 43: if (g_track) {              // RSSetState(pRasterizerState)
                        auto rs = g_raster_states.find(arg(1));
                        // A null state means D3D's defaults, as for the blend and depth states below.
                        g_cur_raster = (rs != g_raster_states.end()) ? rs->second : kRasterDefaut;
                     } break;
            case 45: if (g_track) {              // RSSetScissorRects(Num,pRects)
                        uint32_t n = arg(1), p = arg(2);
                        // Only rectangle 0 matters: it pairs with viewport 0, the only one this shim keeps.
                        // Num = 0 unbinds them all, which leaves D3D11's empty rectangle.
                        for (int i = 0; i < 4; i++)
                            g_sc_now[i] = (n && p) ? (int32_t)win32_mem_read32(p + 4 * i) : 0;
                        g_sc_pose = true;
                        static std::set<uint64_t> vus;   // the first 48 DISTINCT rectangles, not a sample
                        if (vus.size() < 48 &&
                            vus.insert(((uint64_t)(g_sc_now[0] & 0xFFFF) << 48) | ((uint64_t)(g_sc_now[1] & 0xFFFF) << 32) |
                                       ((uint64_t)(g_sc_now[2] & 0xFFFF) << 16) | (uint64_t)(g_sc_now[3] & 0xFFFF)).second)
                            fprintf(stderr, "[CISEAU] rectangle distinct #%zu : %d %d %d %d (image %u)\n", vus.size(),
                                    g_sc_now[0], g_sc_now[1], g_sc_now[2], g_sc_now[3], g_present_frames);
                        if (g_geo && p) for (uint32_t i=0;i<n&&i<16;i++) { uint32_t r = p + i*16;
                            geo_state("S %d %d %d %d\n",
                                (int)win32_mem_read32(r), (int)win32_mem_read32(r+4),
                                (int)win32_mem_read32(r+8), (int)win32_mem_read32(r+12)); }
                     } break;
            // WHO FILLS THE LIGHTING VOLUME? It is created with `data=no` and it is
            // not UpdateSubresource, so the fill path is still unknown — and
            // guessing at it is how an evening disappears. Log every context call
            // that so much as mentions one of these resources.
            if (bo2_getenv("BO2_TEX3D_TRACE") && !g_tex3d_desc.empty()) {
                for (int q = 1; q <= 3; q++) {
                    if (!g_tex3d_desc.count(arg(q))) continue;
                    static uint32_t nt3 = 0;
                    if (++nt3 <= 60)
                        fprintf(stderr, "[TEX3D] context slot %u arg%d=%08x "
                                        "(args %08x %08x %08x %08x %08x %08x)\n",
                                slot, q, arg(q), arg(1), arg(2), arg(3),
                                arg(4), arg(5), arg(6));
                    break;
                }
            }
            // 🎯 GenerateMips(pShaderResourceView) — ID3D11DeviceContext vtable 60.
            // NOT HANDLED. Measured today: [MIP-CENSUS] says every one of ~2000
            // CreateTexture2D calls declares MipLevels 0 or 1, UpdateSubresource
            // never touches a 2D target ([US-2D] never fired), and Map only ever
            // carries video planes — so NO path we intercept delivers a mip chain,
            // and every grazing-angle surface smears. MipLevels=0 in D3D11 means
            // "allocate the full chain", and the chain is then filled by THIS
            // call. If the guest makes it and we drop it, that is the whole story.
            // Count it before building anything.
            case 60: {
                        static uint64_t ngm = 0;
                        if (++ngm <= 8 || (ngm % 1000) == 0)
                            fprintf(stderr, "[GEN-MIPS] guest called GenerateMips #%llu "
                                            "on srv=0x%08x — WE IGNORE IT\n",
                                    (unsigned long long)ngm, arg(1));
                     } break;
            case 48: if (g_window_gfx) {         // UpdateSubresource(pDst,Sub,pBox,
                        //                       pSrc,RowPitch,DepthPitch)
                        // NOT HANDLED AT ALL until now, and it is how the guest
                        // fills the baked-lighting VOLUME: CreateTexture3D reports
                        // `data=no` for it, so its pixels only ever arrive here.
                        // While it stayed empty, `texture3d modelLightingSampler`
                        // resolved to a 1x1 stand-in and the room had no diffuse
                        // lighting at all — which, once the reflection cube became
                        // real, turned every surface into a mirror.
                        uint32_t dst = arg(1), sub = arg(2), box = arg(3);
                        uint32_t src = arg(4), rp = arg(5), dpp = arg(6);
                        // 🎯 EVERYTHING THAT IS NOT A 3D VOLUME IS DROPPED HERE,
                        // and for a 2D texture `sub` IS THE MIP LEVEL. If the game
                        // delivers its mip chain this way, every level above 0 has
                        // been going in the bin in complete silence. Count it.
                        {
                            static uint64_t n2d = 0, nsub = 0, maxsub = 0;
                            if (!g_tex3d_desc.count(dst)) {
                                ++n2d;
                                if (sub) { ++nsub; if (sub > maxsub) maxsub = sub; }
                                if (n2d == 50 || n2d == 500 || (n2d % 5000) == 0)
                                    fprintf(stderr, "[US-2D] UpdateSubresource on NON-3D targets: "
                                                    "%llu calls, %llu with sub>0 (max sub=%llu) — "
                                                    "%s\n",
                                            (unsigned long long)n2d, (unsigned long long)nsub,
                                            (unsigned long long)maxsub,
                                            nsub ? "🎯 THOSE ARE MIP LEVELS AND THEY ARE DROPPED"
                                                 : "all level 0; no mip chain arrives this way");
                            }
                        }
                        auto it3 = g_tex3d_desc.find(dst);
                        if (it3 != g_tex3d_desc.end() && src && !sub && !box) {
                            const Tex3DDesc& T = it3->second;
                            uint32_t rows = (T.fmt >= 70 && T.fmt <= 84) ? (T.h + 3) / 4
                                                                        : T.h;
                            if (!rp) rp = T.w * 4;
                            if (!dpp) dpp = rp * rows;
                            uint64_t nb = (uint64_t)dpp * T.d;
                            if (nb && nb <= (32u << 20)) {
                                std::vector<uint8_t> tb((size_t)nb);
                                win32_mem_read_block(tb.data(), src, (uint32_t)nb);
                                gfx_register_texture_vol(dst, T.w, T.h, T.d, 1, T.fmt,
                                                         tb.data(), (uint32_t)nb, rp, dpp);
                                static uint32_t nu = 0;
                                if (++nu <= 8)
                                    fprintf(stderr, "[gfx] UpdateSubresource -> 3D %08x "
                                                    "%ux%ux%u fmt=%u rp=%u dp=%u\n",
                                            dst, T.w, T.h, T.d, T.fmt, rp, dpp);
                            }
                        }
                     } break;
            // 🎯 CopySubresourceRegion(pDst, DstSub, X,Y,Z, pSrc, SrcSub, pBox)
            // — vtable 46, NEUF arguments, et JAMAIS TRAITE jusqu'ici.
            //
            // Pourquoi ca compte maintenant : l'histogramme des tailles de
            // texture du run w44 plafonne a 1024x1024 (24 textures) et la
            // majorite vit en 256x256 / 128x128, ce qui est absurde pour un AAA
            // de 2012. Une texture de rocher en 256 etiree sur une falaise donne
            // exactement les bandes lisses que le testeur voit. Les deux autres voies
            // d'arrivee ont deja ete mesurees MUETTES sur les cibles 2D :
            // [US-2D] (UpdateSubresource) n'a jamais tire, et [GEN-MIPS]
            // (GenerateMips) vaut 0. Il reste celle-ci, et c'est la voie
            // classique d'un streamer de textures : ecrire le niveau haute
            // definition dans une texture de transfert, puis le copier dans le
            // sous-niveau voulu de la texture finale.
            //
            // On ne repare rien ici : on COMPTE, et on dit vers quoi ca copie et
            // dans quel sous-niveau. Si DstSub > 0 apparait, le niveau de mip est
            // nomme et la cause est etablie ; si l'appel ne tire jamais, cette
            // piste meurt proprement et il faudra chercher ailleurs.
            case 46: {
                        static uint64_t ncsr = 0, n_sub_gt0 = 0, maxsub = 0;
                        static uint64_t to_2d = 0, to_3d = 0, to_dyn = 0, to_unknown = 0;
                        const uint32_t dst = arg(1), dsub = arg(2), src = arg(6),
                                       ssub = arg(7), box = arg(8);
                        ++ncsr;
                        if (dsub) { ++n_sub_gt0; if (dsub > maxsub) maxsub = dsub; }
                        if (g_tex3d_desc.count(dst))      ++to_3d;
                        else if (g_dyntex.count(dst))     ++to_dyn;
                        else if (g_tex2d_wh.count(dst))   ++to_2d;
                        else                              ++to_unknown;
                        if (ncsr <= 6 || ncsr == 100 || ncsr == 1000 || (ncsr % 20000) == 0) {
                            auto dw = g_tex2d_wh.find(dst);
                            auto sw = g_tex2d_wh.find(src);
                            fprintf(stderr, "[CSR #%llu] dst=%08x sub=%u (%u,%u,%u) <- "
                                            "src=%08x sub=%u box=%08x | dst %ux%u fmt=%u "
                                            "mips=%u | src %ux%u fmt=%u\n",
                                    (unsigned long long)ncsr, dst, dsub,
                                    arg(3), arg(4), arg(5), src, ssub, box,
                                    dw == g_tex2d_wh.end() ? 0 : dw->second.w,
                                    dw == g_tex2d_wh.end() ? 0 : dw->second.h,
                                    dw == g_tex2d_wh.end() ? 0 : dw->second.fmt,
                                    dw == g_tex2d_wh.end() ? 0 : dw->second.mips,
                                    sw == g_tex2d_wh.end() ? 0 : sw->second.w,
                                    sw == g_tex2d_wh.end() ? 0 : sw->second.h,
                                    sw == g_tex2d_wh.end() ? 0 : sw->second.fmt);
                            fprintf(stderr, "[CSR] %llu appels ; %llu avec DstSubresource>0 "
                                            "(max=%llu) ; cibles : 2D=%llu 3D=%llu "
                                            "dynamique=%llu inconnue=%llu — %s\n",
                                    (unsigned long long)ncsr, (unsigned long long)n_sub_gt0,
                                    (unsigned long long)maxsub, (unsigned long long)to_2d,
                                    (unsigned long long)to_3d, (unsigned long long)to_dyn,
                                    (unsigned long long)to_unknown,
                                    n_sub_gt0 ? "🎯 DES NIVEAUX DE MIP PASSENT PAR ICI ET ON LES JETTE"
                                              : "tout en sous-niveau 0 : aucune chaine de mip par cette voie");
                        }
                     } break;
            case 47: {                           // CopyResource(pDst, pSrc)
                        // THE MISSING LINK. The frontend composes its frame into an
                        // offscreen target and then COPIES it to the backbuffer —
                        // it does not draw it. Without this the window only ever
                        // showed the handful of draws aimed straight at the
                        // backbuffer, which is why every geometry, shader and
                        // render-target fix left the picture unchanged.
                        {   static uint64_t ncr = 0;
                            if (++ncr <= 3 || ncr % 2000 == 0)
                                fprintf(stderr, "[gfx] CopyResource seen #%llu dst=%08x "
                                                "src=%08x (window=%d)\n",
                                        (unsigned long long)ncr, arg(1), arg(2), g_window_gfx);
                        }
                        // A VOLUME COPY, staging -> dynamic. The room's baked
                        // lighting is a DYNAMIC 3D texture that the guest never
                        // Maps directly: it Maps a STAGING volume, writes it, and
                        // CopyResources it across. Measured — the two staging
                        // volumes (4762d0cc, 4762d10c) upload through Map/Unmap
                        // while the dynamic one (4762d04c), the one actually bound
                        // half a million times a run, stayed empty. The copy path
                        // is the only thing that ever gives it content.
                        bool copie_volume = false;   // déjà faite par l'envoi du volume : la passe de copie 2D n'écrirait que sa tranche 0 (P1103)
                        if (g_window_gfx) {
                            auto sd = g_dyntex.find(arg(2));
                            auto dd = g_dyntex.find(arg(1));
                            if (sd != g_dyntex.end() && dd != g_dyntex.end() &&
                                sd->second.depth > 1 && sd->second.backing &&
                                dd->second.depth == sd->second.depth &&
                                dd->second.bytes == sd->second.bytes) {
                                copie_volume = true;
                                static std::vector<uint8_t> cb2;
                                cb2.resize(sd->second.bytes);
                                win32_mem_read_block(cb2.data(), sd->second.backing,
                                                     sd->second.bytes);
                                gfx_register_texture_vol(arg(1), dd->second.w,
                                                         dd->second.h, dd->second.depth,
                                                         1, dd->second.fmt, cb2.data(),
                                                         (uint32_t)cb2.size(),
                                                         dd->second.pitch,
                                                         dd->second.slice);
                                static uint32_t nvc = 0;
                                if (++nvc <= 6) {
                                    // ⚠️ SAY WHETHER THE VOLUME HAS ANY CONTENT.
                                    // The copy firing proves the PATH works; it
                                    // says nothing about the bytes. "The baked
                                    // lighting volume is still empty" is the
                                    // standing explanation for the room being
                                    // lit only by its reflection probe, and it
                                    // has never been measured — a volume of
                                    // zeros and a volume of real light grid
                                    // produce identical log lines otherwise.
                                    uint32_t mn = 255, mx = 0; uint64_t sum = 0;
                                    const size_t nb = cb2.size();
                                    for (size_t q = 0; q < nb; q++) {
                                        const uint8_t v = cb2[q];
                                        if (v < mn) mn = v;
                                        if (v > mx) mx = v;
                                        sum += v;
                                    }
                                    fprintf(stderr, "[gfx] volume CopyResource %08x <- "
                                                    "%08x %ux%ux%u  bytes=%zu "
                                                    "min=%u max=%u mean=%.1f%s\n",
                                            arg(1), arg(2),
                                            dd->second.w, dd->second.h, dd->second.depth,
                                            nb, mn, mx,
                                            nb ? (double)sum / (double)nb : 0.0,
                                            mx == 0 ? "   <-- ALL ZEROS: the volume IS empty"
                                                    : "   <-- HAS CONTENT");
                                }
                            }
                        }
                        if (g_window_gfx && !copie_volume) {
                            GfxDraw cp{};
                            cp.is_copy = 1;
                            cp.copy_dst = arg(1);
                            cp.copy_src = arg(2);
                            gfx_real_draw(&cp);
                        }
                     } break;
            case 50: {                           // ClearRenderTargetView(pRTV,ColorRGBA[4])
                        uint32_t rtv = arg(1), c = arg(2);
                        if (g_geo && c) geo_state("C %.4f %.4f %.4f %.4f\n",
                                geo_f32(c), geo_f32(c+4), geo_f32(c+8), geo_f32(c+12));
                        // Order matters: a clear sits at a point in the command
                        // stream, so record it IN the frame's list rather than as
                        // per-target state. Without this, offscreen targets load
                        // undefined content and whatever samples them shows garbage.
                        if (g_window_gfx && c) {
                            GfxDraw cl{};
                            cl.is_clear = 1;
                            cl.rtv = rtv;
                            for (int i = 0; i < 4; i++) cl.clear_rgba[i] = geo_f32(c + i * 4);
                            gfx_real_draw(&cl);
                        }
                     } break;
            case 7: if (g_track) {               // VSSetConstantBuffers(Start,Num,ppCBs)
                        uint32_t st0 = arg(1), n = arg(2), pp = arg(3);
                        // ⚠️ SAY IT WHEN A SLOT IS DROPPED. kMaxCB is 8; D3D11
                        // allows 14 per stage, and the clamp below discards
                        // 8..13 SILENTLY. If BO2 puts its bone matrices (or any
                        // per-object transform) up there, every skinned mesh —
                        // the first-person arms and weapon included — gets a
                        // garbage transform and smears, with nothing in the log
                        // to say why. Never let a clamp throw away guest state
                        // without counting it.
                        if (st0 + n > kMaxCB) {
                            static uint32_t nover = 0; static uint32_t hi = 0;
                            if (st0 + n - 1 > hi) hi = st0 + n - 1;
                            if (++nover <= 8 || (nover % 20000) == 0)
                                fprintf(stderr, "[gfx] VS-CB OVER LIMIT: start=%u num=%u "
                                                "-> slots up to %u DROPPED (kMaxCB=%u), "
                                                "occurrence #%u, highest slot seen %u\n",
                                        st0, n, st0 + n - 1, (unsigned)kMaxCB, nover, hi);
                        }
                        for (uint32_t i = 0; i < n && st0 + i < kMaxCB; i++)
                            g_vscb[st0 + i] = pp ? win32_mem_read32(pp + i * 4) : 0;
                        if (st0 == 0) g_ui_vscb0 = g_vscb[0];
                     } break;
            case 16: if (g_track) {              // PSSetConstantBuffers(Start,Num,ppCBs)
                        uint32_t st0 = arg(1), n = arg(2), pp = arg(3);
                        for (uint32_t i = 0; i < n && st0 + i < kMaxCB; i++)
                            g_pscb[st0 + i] = pp ? win32_mem_read32(pp + i * 4) : 0;
                     } break;
            case 17: if (g_track) g_cur_il = arg(1); break;   // IASetInputLayout
            case 33: if (g_track) {              // OMSetRenderTargets(Num,ppRTVs,pDSV)
                        uint32_t pp = arg(2);
                        g_cur_rtv = pp ? win32_mem_read32(pp) : 0;
                        // ...and the DEPTH-STENCIL VIEW, arg 3. Which depth buffer a
                        // pass writes decides whether two passes share one, and the
                        // backend cannot allocate per-pass depth without it.
                        g_cur_dsv = arg(3);
                        g_cur_dsv_res = dsv_pochoir_res(g_cur_dsv);
                        g_cur_dsv_prof = dsv_prof(g_cur_dsv, &g_cur_dsv_wh);
                        // ── 🎯 LB3 — `rtv == 0` EST AMBIGU, ET PERSONNE N'A LU
                        // `NumViews`. La ligne au-dessus ecrase deux situations
                        // que D3D distingue :
                        //   NumViews == 0            -> l'invite DETACHE toute
                        //                               cible couleur. C'est le
                        //                               prologue d'une passe
                        //                               PROFONDEUR SEULE.
                        //   NumViews >= 1, RTV lu 0  -> l'invite lie une vue
                        //                               NULLE dans le tableau.
                        // Le backend traite les deux comme « ecran » (`to_screen
                        // = (rtv == 0) || ...`), donc la passe d'ombre part dans
                        // le drawable : LB2 a mesure 5534 groupes ecran de ce
                        // genre sur 16384, tous avec un dsv non nul, portant
                        // 57,8 % des dessins du run et ne peignant AUCUN pixel.
                        //
                        // Avant de corriger le rendu il faut savoir LAQUELLE des
                        // deux situations le jeu produit — sinon le correctif
                        // repose sur une supposition. Ce recensement repond, ne
                        // change RIEN, et imprime meme a zero : « jamais vu »
                        // doit etre discernable de « la sonde n'a pas tourne ».
                        {
                            static uint64_t om_n, om_nv0, om_nullpp, om_rtv0,
                                            om_ok, om_nv0_dsv, om_rtv0_dsv,
                                            om_nvmax;
                            const uint32_t nv = arg(1);
                            ++om_n;
                            if (nv > om_nvmax) om_nvmax = nv;
                            if (nv == 0)            { ++om_nv0;    if (g_cur_dsv) ++om_nv0_dsv; }
                            else if (!pp)           { ++om_nullpp; }
                            else if (!g_cur_rtv)    { ++om_rtv0;   if (g_cur_dsv) ++om_rtv0_dsv; }
                            else                    { ++om_ok; }
                            // ── 🎯 LA TAILLE DERRIERE LE dsv, RESOLUE POUR DE VRAI.
                            // Le correctif a besoin de (w,h) et de rien d'autre :
                            // depth_for(w,h) alloue a la demande, la texture de
                            // l'invite n'a pas a exister cote Metal. La chaine
                            // `dsv -> g_geo_srv -> res -> g_tex2d_wh` tient PAR
                            // LECTURE (g_tex2d_wh:3197 est rempli des qu'il y a un
                            // descripteur, meme sans pInitialData, donc une carte
                            // d'ombre y est). Mais je viens de me faire retourner
                            // un correctif par une supposition de la meme famille
                            // — « NumViews vaudra 0 » — alors celle-la se mesure
                            // AVANT, pas apres. Histogramme, pas echantillon.
                            if (!g_cur_rtv && g_cur_dsv) {
                                static uint64_t dz_nores, dz_nowh, dz_n[6];
                                static uint32_t dz_w[6], dz_h[6], dz_nv2;
                                static uint64_t dz_deborde;
                                auto gs = g_geo_srv.find(g_cur_dsv);
                                if (gs == g_geo_srv.end()) ++dz_nores;
                                else {
                                    auto wh = g_tex2d_wh.find(gs->second);
                                    if (wh == g_tex2d_wh.end()) ++dz_nowh;
                                    else {
                                        uint32_t t = 0;
                                        for (; t < dz_nv2; t++)
                                            if (dz_w[t] == wh->second.w && dz_h[t] == wh->second.h) break;
                                        if (t < dz_nv2) ++dz_n[t];
                                        else if (dz_nv2 < 6u) {
                                            dz_w[t] = wh->second.w; dz_h[t] = wh->second.h;
                                            dz_n[t] = 1u; ++dz_nv2;
                                        } else ++dz_deborde;
                                    }
                                }
                                if ((om_rtv0 & (om_rtv0 - 1)) == 0) {
                                    fprintf(stderr, "[DSV-TAILLE] sur %llu passes sans couleur : "
                                            "dsv inconnu de g_geo_srv=%llu%s  taille inconnue de "
                                            "g_tex2d_wh=%llu%s  |  tailles resolues :",
                                            (unsigned long long)om_rtv0,
                                            (unsigned long long)dz_nores,
                                            dz_nores ? "  🔴 <<< LA CHAINE CASSE ICI" : "",
                                            (unsigned long long)dz_nowh,
                                            dz_nowh ? "  🔴 <<< LA CHAINE CASSE ICI" : "");
                                    if (!dz_nv2) fprintf(stderr, "  AUCUNE — le correctif n'a pas sa taille");
                                    for (uint32_t t = 0; t < dz_nv2; t++)
                                        fprintf(stderr, "  %ux%u x%llu", dz_w[t], dz_h[t],
                                                (unsigned long long)dz_n[t]);
                                    if (dz_deborde)
                                        fprintf(stderr, "  (+%llu hors des 6 premieres tailles)",
                                                (unsigned long long)dz_deborde);
                                    fprintf(stderr, "\n");
                                }
                            }
                            if ((om_n & (om_n - 1)) == 0)
                                fprintf(stderr,
                                        "[OMSET-LB3] appels=%llu  NumViews=0 (detache tout)=%llu "
                                        "(dont dsv non nul=%llu)%s  |  NumViews>=1 mais ppRTVs NUL=%llu%s "
                                        " |  NumViews>=1 et RTV lu NUL=%llu (dont dsv non nul=%llu)%s "
                                        " |  cible reelle=%llu  |  NumViews max vu=%llu\n",
                                        (unsigned long long)om_n,
                                        (unsigned long long)om_nv0,
                                        (unsigned long long)om_nv0_dsv,
                                        om_nv0 ? "  \xf0\x9f\x8e\xaf <<< C'EST LE CAS PROFONDEUR SEULE"
                                               : "  <<< aucun : la passe d'ombre ne vient PAS de la",
                                        (unsigned long long)om_nullpp,
                                        om_nullpp ? "  <<< NumViews ment sur ppRTVs" : "",
                                        (unsigned long long)om_rtv0,
                                        (unsigned long long)om_rtv0_dsv,
                                        om_rtv0 ? "  \xf0\x9f\x8e\xaf <<< VUE NULLE LIEE : autre cas, autre correctif"
                                                : "  <<< aucun",
                                        (unsigned long long)om_ok,
                                        (unsigned long long)om_nvmax);
                        }
                     } break;
            case 35: if (g_track) {              // OMSetBlendState(pState,Factor,Mask)
                        uint32_t bs = arg(1);
                        auto b = g_blend_states.find(bs);
                        // A null state means "D3D defaults", not "keep the last".
                        g_cur_blend = (b != g_blend_states.end()) ? b->second
                                                                  : kBlendDefault;
                     } break;
            case 53: if (g_track) {              // ClearDepthStencilView(pDSV,Flags,Depth,Stencil)
                        // ClearFlags: D3D11_CLEAR_DEPTH = 1, D3D11_CLEAR_STENCIL = 2 (the game passes its command
                        // byte & 3, t6sp 0x6c4016). The stencil half becomes a record at its place in the stream;
                        // the depth half keeps its old path (one global value, first pass of each size).
                        const uint32_t fl = arg(2);
                        const uint32_t st_res = (fl & 2) ? dsv_pochoir_res(arg(1)) : 0;
                        if (st_res && g_pochoir && g_window_gfx) {
                            GfxDraw cl{};
                            cl.is_clear = 2;             // 2 = stencil plane of depth.st_res (1 = ClearRenderTargetView)
                            cl.rtv = ~0u;                // neither the screen (0) nor a target: joins no group
                            cl.depth.st_res = st_res;
                            cl.depth.st_ref = arg(4) & 0xFFu;   // a UINT8 pushed as a dword: only the low byte is defined
                            gfx_real_draw(&cl);
                        }
                        {
                            static uint64_t nfl[4], tot;
                            ++nfl[fl & 3];
                            if (puiss2(++tot))
                                fprintf(stderr, "[POCHOIR] ClearDepthStencilView=%llu : drapeaux 0/1/2/3 = "
                                                "%llu/%llu/%llu/%llu (dernier dsv=%08x plan de pochoir=%08x)\n",
                                        (unsigned long long)tot, (unsigned long long)nfl[0],
                                        (unsigned long long)nfl[1], (unsigned long long)nfl[2],
                                        (unsigned long long)nfl[3], arg(1), st_res);
                        }
                        if (g_pochoir && !(fl & 1)) break;   // BO2_STENCIL=0 : exactly the path of before
                        uint32_t bits = arg(3);
                        float dv; memcpy(&dv, &bits, 4);
                        if (fl & 1) {   // the depth half as a record at its place in the stream, like the stencil half above
                            uint32_t wh = 0;
                            const uint32_t pr = g_window_gfx ? dsv_prof(arg(1), &wh) : 0;
                            if (pr) {
                                GfxDraw cl{};
                                cl.is_clear = 3; cl.rtv = ~0u; cl.dsv_res = pr; cl.dsv_wh = wh;
                                cl.clear_rgba[0] = dv > 1.f ? 1.f : dv >= 0.f ? dv : 0.f;   // D3D11 clamps to [0, 1]
                                gfx_real_draw(&cl);
                            }
                        }
                        if (dv >= 0.0f && dv <= 1.0f) {
                            static int ndc = 0;
                            if (g_depth_clear != dv && ++ndc <= 8)
                                fprintf(stderr, "[D3D-DEPTH] ClearDepthStencilView "
                                                "depth=%.3f (was %.3f)\n", dv, g_depth_clear);
                            g_depth_clear = dv;
                        }
                     } break;
            case 36: if (g_track) {              // OMSetDepthStencilState(pState,StencilRef)
                        auto z = g_depth_states.find(arg(1));
                        // A null state means D3D's defaults, not "keep the last" —
                        // the same rule OMSetBlendState above follows.
                        g_cur_depth = (z != g_depth_states.end()) ? z->second
                                                                  : kDepthDefault;
                        if (g_cur_depth.st) {    // StencilRef belongs to the BINDING, not to the state object
                            g_cur_depth.st_ref = arg(2) & 0xFFu;
                            static uint64_t nl = 0; static uint32_t rmax = 0;
                            if (g_cur_depth.st_ref > rmax) rmax = g_cur_depth.st_ref;
                            if (puiss2(++nl))
                                fprintf(stderr, "[POCHOIR] liaisons a pochoir=%llu etat=%08x ref=%u ref_max=%u\n",
                                        (unsigned long long)nl, arg(1), g_cur_depth.st_ref, rmax);
                        }
                     } break;
            case 24: if (g_track) g_ui_topo = arg(1); break;  // IASetPrimitiveTopology
            case 10: if (g_track) {              // PSSetSamplers(StartSlot,NumSamplers,ppSamplers) — P805
                        uint32_t st0 = arg(1), nn = arg(2), pp2 = arg(3);
                        for (uint32_t i = 0; i < nn && st0 + i < kMaxPSTex; i++)
                            g_pssamp[st0 + i] = pp2 ? win32_mem_read32(pp2 + i * 4) : 0;
                     } break;
            case 9: case 11: {                   // PSSetShader / VSSetShader
                uint32_t so = arg(1);
                if (g_track) {
                    if (slot == 9) g_cur_ps = so;
                    else { g_cur_vs = so; g_cur_vs_vid = !so || vs_reads_vertex_id(so); }   // C46
                }
                if (!so) break;
                static const int dump = bo2_getenv("BO2_SHADER_DUMP") ? 1 : 0;
                static std::unordered_map<uint32_t,int> bound;
                if (!bound[so]++ && dump)
                    fprintf(stderr, "[SHADER-BOUND] %s obj=%08x distinct bound=%zu"
                                    " (created=%zu)\n",
                            slot == 9 ? "PS" : "VS", so, bound.size(), g_sh_obj.size());
                // Translate on FIRST BIND: only the shaders the menu really uses
                // pay for it, and the cost is spread over frames instead of one
                // 2518-shader burst that trips the stall watchdog.
                translate_shader(so, slot);
                break; }
            case 8: if (g_track) {               // PSSetShaderResources(Start,Num,ppSRVs)
                        // Record EVERY bind, including a non-zero StartSlot: the old
                        // handler dropped those entirely, and the font draws arrive with
                        // no texture at all.
                        g_last_srv_start = arg(1);
                        g_last_srv_num   = arg(2);
                        g_last_srv_obj   = arg(3) ? win32_mem_read32(arg(3)) : 0;
                        uint32_t st0 = arg(1), nn = arg(2), pp2 = arg(3);
                        for (uint32_t i = 0; i < nn && st0 + i < kMaxPSTex; i++) {
                            uint32_t sv = pp2 ? win32_mem_read32(pp2 + i * 4) : 0;
                            auto f2 = g_geo_srv.find(sv);
                            g_pstex[st0 + i] = (f2 != g_geo_srv.end()) ? f2->second : 0;
                            auto fm = sv ? g_srv_mip.find(sv) : g_srv_mip.end();
                            g_psmip[st0 + i] = fm != g_srv_mip.end() ? fm->second : 0;
                        }
                        // Le contexte TIENT chaque vue liee (dref_release) : la
                        // nouvelle gagne une reference, celle qu'elle remplace la perd.
                        for (uint32_t i = 0; i < nn && st0 + i < 128; i++) {
                            uint32_t nv = pp2 ? win32_mem_read32(pp2 + i * 4) : 0;
                            uint32_t av = g_dref_ps[st0 + i];
                            if (nv == av) continue;
                            g_dref_ps[st0 + i] = nv;
                            dref_add(nv);
                            dref_release(av);
                        }
                        // WHOSE FAULT IS Cb == Cr? The cinematic draw resolves its
                        // four plane slots to only TWO distinct textures. That is
                        // either the game binding one SRV into two slots (its own
                        // business, and then the planes are packed some other way)
                        // or two DIFFERENT SRVs collapsing onto one resource in
                        // this shim — the fourth appearance of a shape that has
                        // already cost this project three bugs. Printing the SRV
                        // ids NEXT TO the resources they resolve to separates them
                        // in one line; the resolved ids alone cannot. BO2_WEBM=1.
                        if (bo2_getenv("BO2_WEBM") && nn >= 4 && st0 == 0) {
                            static uint32_t nsrv = 0;
                            if (++nsrv <= 6 || (nsrv % 4000) == 0) {
                                fprintf(stderr, "[SRV4 #%u] start=%u num=%u |", nsrv, st0, nn);
                                for (uint32_t i = 0; i < nn && i < 4; i++) {
                                    uint32_t sv = pp2 ? win32_mem_read32(pp2 + i * 4) : 0;
                                    auto f3 = g_geo_srv.find(sv);
                                    fprintf(stderr, " [%u] srv=%08x -> res=%08x", i, sv,
                                            f3 != g_geo_srv.end() ? f3->second : 0);
                                }
                                fprintf(stderr, "\n");
                            }
                        }
                     }
                     if (g_track && arg(1) == 0) {
                        uint32_t pp = arg(3);
                        uint32_t srv = pp ? win32_mem_read32(pp) : 0;
                        auto s = g_geo_srv.find(srv);
                        g_geo_ps_tex = (s != g_geo_srv.end()) ? s->second : 0;
                        // (the slot array itself is filled above, for EVERY StartSlot)
                        // The text draws reported tex=0, i.e. this lookup misses.
                        // Distinguish the three possible causes instead of guessing:
                        // no SRV bound at all, an SRV we never saw created, or an
                        // SRV whose resource is a texture we have no pixels for.
                        static uint32_t nmiss = 0, nhit = 0, nnull = 0;
                        if (!srv) nnull++; else if (g_geo_ps_tex) nhit++; else nmiss++;
                        if (((nmiss + nhit + nnull) % 2000) == 1)
                            fprintf(stderr, "[SRV] bind hits=%u misses(unknown SRV)=%u "
                                            "null=%u (srv objs known=%zu)\n",
                                    nhit, nmiss, nnull, g_geo_srv.size());
                     } break;
            case 18: if (g_track) {              // IASetVertexBuffers(Start,Num,ppVB,pStrides,pOffsets)
                        // ⚠️ THIS IGNORED StartSlot AND ONLY EVER READ ppVB[0].
                        // The engine binds the room's geometry as SEVERAL STREAMS —
                        // slot 0 positions (stride 32/36), slot 1 a packed 4-byte
                        // per-vertex attribute — and a bind of slot 1 therefore
                        // OVERWROTE the record for slot 0. The next draw then read
                        // POSITIONS with a stride of 4 and produced vertices like
                        // 1.18e37. Measured over one menu frame: 345437 of 747241
                        // draws into the scene target, 46%, had stride=4.
                        uint32_t start = arg(1), num = arg(2);
                        uint32_t ppVB = arg(3), pStr = arg(4), pOff = arg(5);
                        for (uint32_t i = 0; i < num && start + i < kMaxVBSlots; i++) {
                            uint32_t sl = start + i;
                            g_vb_slot[sl]    = ppVB ? win32_mem_read32(ppVB + i * 4) : 0;
                            g_vb_stride[sl]  = pStr ? win32_mem_read32(pStr + i * 4) : 0;
                            g_vb_off[sl]     = pOff ? win32_mem_read32(pOff + i * 4) : 0;
                        }
                        // Slot 0 remains what the single-stream draw path reads.
                        g_geo_vb        = g_vb_slot[0];
                        g_geo_vb_stride = g_vb_stride[0];
                        g_geo_vb_off    = g_vb_off[0];
                        g_ui_vb = g_geo_vb; g_ui_stride = g_geo_vb_stride;
                        g_ui_vboff = g_geo_vb_off;
                     } break;
            case 19: if (g_track) {              // IASetIndexBuffer(pIB,Format,Offset)
                        g_geo_ib     = arg(1);
                        g_geo_ib_fmt = arg(2);
                        g_geo_ib_off = arg(3);
                        g_ui_ib = g_geo_ib; g_ui_ibfmt = g_geo_ib_fmt;
                        g_ui_iboff = g_geo_ib_off;
                     } break;
            case 74: case 76: case 78: case 82: case 94:
            case 98: case 102: case 107: {       // *GetShader/IAGetInputLayout/
                uint32_t pp = arg(1);            // RSGetState -> nothing bound
                if (pp) win32_mem_write32(pp, 0);
                break; }
            case 83: { uint32_t p = arg(1);      // IAGetPrimitiveTopology
                       if (p) win32_mem_write32(p, 0);
                       break; }
            case 89: { uint32_t n = arg(1), ppRTV = arg(2), ppDSV = arg(3);
                       if (ppRTV) for (uint32_t i = 0; i < n && i < 8; i++)
                           win32_mem_write32(ppRTV + i * 4, 0);
                       if (ppDSV) win32_mem_write32(ppDSV, 0);
                       break; }                  // OMGetRenderTargets
            default: break;
        }
        static std::unordered_map<int, int> ctx_seen;
        if (ctx_seen[slot]++ == 0)
            fprintf(stderr, "[D3D-CTX] slot=%d nargs=%d ret=0x%08x\n",
                    slot, nargs, win32_mem_read32(esp));
        ret(0, nargs);
        return true;
    }

    // Unimplemented: log (iface/slot/caller) and return S_OK, cleaning only
    // `this`. If the method took more stack args this leaks — add a handler with
    // the real arity the moment it fires (measure -> implement).
    static std::unordered_map<uint32_t, int> seen;
    if (seen[addr]++ == 0)
        fprintf(stderr, "[D3D-UNIMPL] iface=%d slot=%d ret=0x%08x this=0x%08x -> S_OK(clean1)\n",
                iface, slot, win32_mem_read32(esp), arg(0));
    ret(0, 1);
    return true;
}

// --- faultrep (1 function) ---
DLL_STUB2(ReportFault)

// --- PSAPI (1 function) ---
// GetProcessMemoryInfo is already in kernel32_impls (it's PSAPI but
// dispatched by name). No additional stub needed.

// --- REGISTRATION ---
static const ImportEntry remaining_dlls_impls[] = {
    // GDI32
    {"DeleteDC", impl_DeleteDC},
    {"DeleteObject", impl_DeleteObject},
    {"GetBitmapBits", impl_GetBitmapBits},
    {"BitBlt", impl_BitBlt},
    {"GetObjectA", impl_GetObjectA},
    {"SelectObject", impl_SelectObject},
    {"CreateSolidBrush", impl_CreateSolidBrush},
    {"CreateCompatibleBitmap", impl_CreateCompatibleBitmap},
    {"CreateCompatibleDC", impl_CreateCompatibleDC},
    {"CreateDCA", impl_CreateDCA},
    {"GetDeviceCaps", impl_GetDeviceCaps},
    {"CreateFontA", impl_CreateFontA},
    {"SetDeviceGammaRamp", impl_SetDeviceGammaRamp},
    // ADVAPI32
    {"SetSecurityDescriptorDacl", impl_SetSecurityDescriptorDacl},
    {"InitializeSecurityDescriptor", impl_InitializeSecurityDescriptor},
    {"RegCloseKey", impl_RegCloseKey},
    {"RegisterEventSourceA", impl_RegisterEventSourceA},
    {"ReportEventA", impl_ReportEventA},
    {"DeregisterEventSource", impl_DeregisterEventSource},
    {"CryptGenRandom", impl_CryptGenRandom},
    {"CryptReleaseContext", impl_CryptReleaseContext},
    {"CryptAcquireContextA", impl_CryptAcquireContextA},
    {"RegQueryValueExW", impl_RegQueryValueExW},
    {"RegOpenKeyExW", impl_RegOpenKeyExW},
    // WINMM
    {"timeBeginPeriod", impl_timeBeginPeriod},
    {"timeEndPeriod", impl_timeEndPeriod},
    {"timeGetTime", impl_timeGetTime},
    {"mixerGetLineControlsA", impl_mixerGetLineControlsA},
    {"mixerOpen", impl_mixerOpen},
    {"mixerSetControlDetails", impl_mixerSetControlDetails},
    {"mixerGetDevCapsA", impl_mixerGetDevCapsA},
    {"waveInGetNumDevs", impl_waveInGetNumDevs},
    {"mixerGetNumDevs", impl_mixerGetNumDevs},
    {"mixerClose", impl_mixerClose},
    {"mixerGetControlDetailsA", impl_mixerGetControlDetailsA},
    {"mixerGetLineInfoA", impl_mixerGetLineInfoA},
    // binkw32
    {"_BinkSetSoundTrack@8", impl__BinkSetSoundTrack_at_8},
    {"_BinkDoFrame@4", impl__BinkDoFrame_at_4},
    {"_BinkClose@4", impl__BinkClose_at_4},
    {"_BinkStartAsyncThread@8", impl__BinkStartAsyncThread_at_8},
    {"_BinkGetError@0", impl__BinkGetError_at_0},
    {"_BinkOpen@8", impl__BinkOpen_at_8},
    {"_BinkWaitStopAsyncThread@4", impl__BinkWaitStopAsyncThread_at_4},
    {"_BinkGetRealtime@12", impl__BinkGetRealtime_at_12},
    {"_BinkSetMemory@8", impl__BinkSetMemory_at_8},
    {"_BinkDoFrameAsyncWait@8", impl__BinkDoFrameAsyncWait_at_8},
    {"_BinkGetRects@8", impl__BinkGetRects_at_8},
    {"_BinkRequestStopAsyncThread@4", impl__BinkRequestStopAsyncThread_at_4},
    {"_BinkRegisterFrameBuffers@8", impl__BinkRegisterFrameBuffers_at_8},
    {"_BinkNextFrame@4", impl__BinkNextFrame_at_4},
    {"_BinkGetFrameBuffersInfo@8", impl__BinkGetFrameBuffersInfo_at_8},
    {"_BinkPause@8", impl__BinkPause_at_8},
    {"_BinkControlBackgroundIO@8", impl__BinkControlBackgroundIO_at_8},
    {"_BinkDoFrameAsync@12", impl__BinkDoFrameAsync_at_12},
    {"_BinkSetIOSize@4", impl__BinkSetIOSize_at_4},
    // steam_api
    {"SteamAPI_Init", impl_SteamAPI_Init},
    {"SteamAPI_RegisterCallResult", impl_SteamAPI_RegisterCallResult},
    {"SteamAPI_RegisterCallback", impl_SteamAPI_RegisterCallback},
    {"SteamUserStats", impl_SteamUserStats},
    {"SteamAPI_Shutdown", impl_SteamAPI_Shutdown},
    {"SteamAPI_UnregisterCallback", impl_SteamAPI_UnregisterCallback},
    {"SteamAPI_UnregisterCallResult", impl_SteamAPI_UnregisterCallResult},
    {"SteamAPI_RunCallbacks", impl_SteamAPI_RunCallbacks},
    {"SteamUtils", impl_SteamUtils},
    {"SteamApps", impl_SteamApps},
    {"SteamFriends", impl_SteamFriends},
    {"SteamNetworking", impl_SteamNetworking},
    {"SteamGameServer_Init", impl_SteamGameServer_Init},
    {"SteamGameServer", impl_SteamGameServer},
    {"SteamGameServer_RunCallbacks", impl_SteamGameServer_RunCallbacks},
    {"SteamGameServer_Shutdown", impl_SteamGameServer_Shutdown},
    {"SteamAPI_RestartAppIfNecessary", impl_SteamAPI_RestartAppIfNecessary},
    {"SteamUser", impl_SteamUser},
    // ole32
    {"StringFromGUID2", impl_StringFromGUID2},
    {"CoCreateInstance", impl_CoCreateInstance},
    {"CoUninitialize", impl_CoUninitialize},
    {"CLSIDFromString", impl_CLSIDFromString},
    {"CoInitializeEx", impl_CoInitializeEx},
    {"CoInitialize", impl_CoInitialize},
    // SHELL32
    {"ShellExecuteA", impl_ShellExecuteA},
    {"SHGetFolderPathA", impl_SHGetFolderPathA},
    // WTSAPI32
    {"WTSUnRegisterSessionNotification", impl_WTSUnRegisterSessionNotification},
    {"WTSRegisterSessionNotification", impl_WTSRegisterSessionNotification},
    // d3d11
    {"D3D11CreateDeviceAndSwapChain", impl_D3D11CreateDeviceAndSwapChain},
    {"D3D11CreateDevice", impl_D3D11CreateDevice},
    // dxgi
    {"CreateDXGIFactory1", impl_CreateDXGIFactory1},
    // faultrep
    {"ReportFault", impl_ReportFault},
    // XINPUT1_3 (by ordinal — DLL-qualified key; win32_shim_init looks ordinals
    // up as "<dll>!Ordinal_N". Arities are the documented XInput API.)
    {"XINPUT1_3_dll!Ordinal_2", impl_xinput_ordinal_2},  // XInputGetState (2 args)
    {"XINPUT1_3_dll!Ordinal_3", impl_xinput_ordinal_3},  // XInputSetState (2 args)
    {"XINPUT1_3_dll!Ordinal_4", impl_xinput_ordinal_4},  // XInputGetCapabilities (3 args)
    // DSOUND (by ordinal) — DLL-qualified keys "<dll>!Ordinal_N" so the
    // shim's name-lookup matches (the previous "#6"/"#11" keys were never
    // found, sending DirectSoundCreate / DirectSoundCaptureCreate down the
    // unimpl stub — eax=0=DS_OK with NULL out-param cascaded into NULL-CALLs
    // + Com_Error("x angle>")). Real impls return DSERR_NODRIVER + zero
    // *ppDS / *ppDSC so the game's "no DirectSound" audio fallback fires.
    {"DSOUND_dll!Ordinal_6",  impl_dsound_ordinal_6},    // DirectSoundCreate (3 args)
    {"DSOUND_dll!Ordinal_11", impl_dsound_ordinal_11},   // DirectSoundCaptureCreate (3 args)
    // WSOCK32 / WS2_32 (by ordinal — DLL-qualified keys, win32_shim_init looks
    // ordinals up as "<dll>!Ordinal_N"). Offline stubs return 0; the ONLY thing
    // that matters here is the correct stdcall arg-pop count — leaving these
    // unregistered fell through to the default stub (pops just the retaddr),
    // which leaked each call's args and cascaded into IAT-sentinel corruption.
    // wsock32 forwards to ws2_32, so both share the standard ordinal numbering.
    {"WSOCK32_dll!Ordinal_2",   impl_wsock_ordinal_2},    // bind(3)
    {"WSOCK32_dll!Ordinal_3",   impl_wsock_ordinal_3},    // closesocket(1)
    {"WSOCK32_dll!Ordinal_9",   impl_wsock_ordinal_9},    // htons(1)
    {"WSOCK32_dll!Ordinal_10",  impl_wsock_ordinal_10},   // inet_addr(1)
    {"WSOCK32_dll!Ordinal_12",  impl_wsock_ordinal_12},   // ioctlsocket(3)
    {"WSOCK32_dll!Ordinal_16",  impl_wsock_ordinal_16},   // recv(4)
    {"WSOCK32_dll!Ordinal_17",  impl_wsock_ordinal_17},   // recvfrom(6)
    {"WSOCK32_dll!Ordinal_18",  impl_wsock_ordinal_18},   // select(5)
    {"WSOCK32_dll!Ordinal_19",  impl_wsock_ordinal_19},   // send(4)
    {"WSOCK32_dll!Ordinal_20",  impl_wsock_ordinal_20},   // sendto(6)
    {"WSOCK32_dll!Ordinal_21",  impl_wsock_ordinal_21},   // setsockopt(5)
    {"WSOCK32_dll!Ordinal_22",  impl_wsock_ordinal_22},   // shutdown(2)
    {"WSOCK32_dll!Ordinal_23",  impl_wsock_ordinal_23},   // socket(3)
    {"WSOCK32_dll!Ordinal_52",  impl_wsock_ordinal_52},   // gethostbyname(1)
    {"WSOCK32_dll!Ordinal_57",  impl_wsock_ordinal_57},   // gethostname(2)
    {"WSOCK32_dll!Ordinal_111", impl_wsock_ordinal_111},  // WSAGetLastError(0)
    {"WSOCK32_dll!Ordinal_115", impl_wsock_ordinal_115},  // WSAStartup(2)
    {"WSOCK32_dll!Ordinal_151", impl_wsock_ordinal_151},  // __WSAFDIsSet(2)
    {"WSOCK32_dll!Ordinal_8",   impl_wsock_ordinal_8},    // htonl(1)
    {"WSOCK32_dll!Ordinal_14",  impl_wsock_ordinal_14},   // ntohl(1)
    {"WSOCK32_dll!Ordinal_15",  impl_wsock_ordinal_15},   // ntohs(1)
    {"WSOCK32_dll!Ordinal_116", impl_wsock_ordinal_116},  // WSACleanup(0)
    {"WSOCK32_dll!Ordinal_4",   impl_wsock_ordinal_4},    // connect(3)
    {"WS2_32_dll!Ordinal_4",    impl_wsock_ordinal_4},    // connect(3) : t6sp l'importe de WS2_32 (P928)
    {"WS2_32_dll!Ordinal_6",    impl_ws2_ordinal_6},      // getsockname(3)
    {"WS2_32_dll!Ordinal_12",   impl_ws2_ordinal_12},     // ioctlsocket(3)
    // inet_ntoa(1) : les exes Steam l'importent de WSOCK32 (ordinal 11). Sans elle, chaîne
    // illisible (« Binding to ﾭ� ») puis tableau des hôtes STUN détruit sur un slot d'import (P973).
    {"WSOCK32_dll!Ordinal_11",  impl_ws2_ordinal_12},
    {"WS2_32_dll!Ordinal_15",   impl_ws2_ordinal_15},     // ntohs(1)
    {"WS2_32_dll!Ordinal_112",  impl_ws2_ordinal_112},    // WSASetLastError(1)
    {nullptr, nullptr},
};

extern "C" const ImportEntry* get_remaining_dlls_impls() {
    return remaining_dlls_impls;
}
