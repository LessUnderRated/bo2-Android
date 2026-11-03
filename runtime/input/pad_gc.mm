// pad_gc.mm — la manette vue par XInput : une vraie (GameController.framework : DualSense, Xbox,
// MFi) ou une scriptee (BO2_PAD_SCRIPT, pour les runs d'acceptation « entrees livrees »).
//
// Le jeu appelle XInputGetState(idx, &XINPUT_STATE) depuis son fil d'entrees ; le shim
// (remaining_dlls_impl.cpp, xinput1_3 ordinaux 2/3/4) demande ici l'etat et l'ecrit en memoire
// invitee. Ici, pas de memoire invitee : des octets au format Windows.
//
//   XINPUT_STATE   = { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; }            16 octets
//   XINPUT_GAMEPAD = { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
//                      SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; }            12 octets
//   dwPacketNumber ne change QUE quand l'etat change (le jeu s'en sert pour sauter une lecture).
//
// Correspondance par POSITION (comme le pilote Xbox de Windows et Steam Input) : Croix = A,
// Rond = B, Carre = X, Triangle = Y ; Options = START, Creer = BACK ; L1/R1 = epaules ;
// L2/R2 = gachettes 0..255 ; sticks -32768..32767, Y vers le haut positif (GameController aussi).
// Les zones mortes sont celles du JEU (XINPUT_GAMEPAD_*_THUMB_DEADZONE lues par lui) : on rend la
// valeur brute, sans en ajouter.
//
// Fils : GameController livre ses changements sur une file serie dediee (handlerQueue), jamais sur
// le fil AppKit (occupe par la pompe de rendu). L'etat est une photo sous mutex ; le fil invite la
// lit. Rien ici ne bloque le fil invite plus que le temps d'une copie de 16 octets.
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "../win32/bo2_getenv.h"

extern "C" void bo2_cmd_queue(const char* cmd);   // win32_runtime.cpp : file des commandes console

namespace {

enum : uint16_t {
    XI_UP = 0x0001, XI_DOWN = 0x0002, XI_LEFT = 0x0004, XI_RIGHT = 0x0008,
    XI_START = 0x0010, XI_BACK = 0x0020, XI_LTHUMB = 0x0040, XI_RTHUMB = 0x0080,
    XI_LSHOULDER = 0x0100, XI_RSHOULDER = 0x0200,
    XI_A = 0x1000, XI_B = 0x2000, XI_X = 0x4000, XI_Y = 0x8000,
};
constexpr uint32_t kSucces = 0, kNonBranchee = 0x48F;   // ERROR_DEVICE_NOT_CONNECTED

struct Gamepad {   // la disposition exacte de XINPUT_GAMEPAD (12 octets, sans bourrage)
    uint16_t boutons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
static_assert(sizeof(Gamepad) == 12, "XINPUT_GAMEPAD fait 12 octets");

struct Etape { double t; Gamepad g; };   // BO2_PAD_SCRIPT : a partir de t secondes, l'etat g

std::mutex g_mu;
Gamepad g_etat;
bool g_branchee = false;
uint32_t g_paquet = 0;
GCController* g_ctl = nil;
dispatch_queue_t g_file = nullptr;
std::vector<Etape> g_script;
bool g_script_actif = false;
std::chrono::steady_clock::time_point g_top;
bool g_top_donne = false;

int16_t axe(float v) {
    v = v < -1.f ? -1.f : v > 1.f ? 1.f : v;
    return (int16_t)(v < 0 ? v * 32768.f : v * 32767.f);
}
uint8_t gachette(float v) {
    v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    return (uint8_t)(v * 255.f + .5f);
}

void publier(const Gamepad& e) {   // sous g_mu
    if (std::memcmp(&e, &g_etat, sizeof e) != 0) { g_etat = e; ++g_paquet; }
}

// `gpad_enabled` est archivee et vaut 0 par defaut (0x4761b2) : le jeu ignore la manette tant que le joueur ne l'a
// pas activee dans Options. Notre config n'etant pas relue, une manette branchee l'active (au branchement, puis au
// top du niveau mesure).
void activer_gpad() {
    bo2_cmd_queue("set gpad_enabled 1");
}

void lire(GCExtendedGamepad* g) {
    Gamepad e;
    auto b = [&](GCControllerButtonInput* x, uint16_t bit) { if (x && x.pressed) e.boutons |= bit; };
    b(g.dpad.up, XI_UP); b(g.dpad.down, XI_DOWN); b(g.dpad.left, XI_LEFT); b(g.dpad.right, XI_RIGHT);
    b(g.buttonMenu, XI_START); b(g.buttonOptions, XI_BACK);
    b(g.leftThumbstickButton, XI_LTHUMB); b(g.rightThumbstickButton, XI_RTHUMB);
    b(g.leftShoulder, XI_LSHOULDER); b(g.rightShoulder, XI_RSHOULDER);
    b(g.buttonA, XI_A); b(g.buttonB, XI_B); b(g.buttonX, XI_X); b(g.buttonY, XI_Y);
    e.lt = gachette(g.leftTrigger.value);  e.rt = gachette(g.rightTrigger.value);
    e.lx = axe(g.leftThumbstick.xAxis.value);  e.ly = axe(g.leftThumbstick.yAxis.value);
    e.rx = axe(g.rightThumbstick.xAxis.value); e.ry = axe(g.rightThumbstick.yAxis.value);
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_script_actif) publier(e);
}

void brancher(GCController* c) {
    if (!c.extendedGamepad) {
        fprintf(stderr, "[PAD] ignoree (pas de profil etendu) : %s\n", c.vendorName.UTF8String ?: "?");
        return;
    }
    { std::lock_guard<std::mutex> lk(g_mu);
      if (g_ctl) return;                       // une seule manette, index 0
      g_ctl = c; g_branchee = true; ++g_paquet; }
    c.handlerQueue = g_file;
    c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad* g, GCControllerElement*) { lire(g); };
    GCExtendedGamepad* g = c.extendedGamepad;
    dispatch_async(g_file, ^{ lire(g); });
    fprintf(stderr, "[PAD] manette branchee sur l'index 0 : %s (%s)\n",
            c.vendorName.UTF8String ?: "?", c.productCategory.UTF8String ?: "?");
    activer_gpad();
}

void debrancher(GCController* c) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (c != g_ctl) return;
    g_ctl = nil; g_branchee = false; g_etat = Gamepad{}; ++g_paquet;
    fprintf(stderr, "[PAD] manette debranchee\n");
}

// Une ligne par etape : `<secondes> <boutons hex> <lt> <rt> <lx> <ly> <rx> <ry>` ; `#` commente.
// Les secondes comptent depuis bo2_pad_top() (le runtime l'appelle au [DB-LOADXFILE] du niveau).
void charger_script(const char* chemin) {
    FILE* f = fopen(chemin, "r");
    if (!f) { fprintf(stderr, "[PAD] REFUS : BO2_PAD_SCRIPT=%s illisible\n", chemin); return; }
    char ln[256];
    int n = 0;
    while (fgets(ln, sizeof ln, f)) {
        n++;
        if (ln[0] == '#' || ln[0] == '\n') continue;
        double t; unsigned bt; int lt, rt, lx, ly, rx, ry;
        if (sscanf(ln, "%lf %x %d %d %d %d %d %d", &t, &bt, &lt, &rt, &lx, &ly, &rx, &ry) != 8) {
            fprintf(stderr, "[PAD] REFUS : BO2_PAD_SCRIPT ligne %d illisible\n", n);
            fclose(f); g_script.clear(); return;
        }
        Gamepad g;
        g.boutons = (uint16_t)bt; g.lt = (uint8_t)lt; g.rt = (uint8_t)rt;
        g.lx = (int16_t)lx; g.ly = (int16_t)ly; g.rx = (int16_t)rx; g.ry = (int16_t)ry;
        g_script.push_back({t, g});
    }
    fclose(f);
    g_script_actif = !g_script.empty();
    fprintf(stderr, "[PAD] script %s : %zu etapes, index 0 branche, manette reelle ignoree\n",
            chemin, g_script.size());
}

uint64_t g_lus = 0;         // XInputGetState(0) depuis le lancement (sous g_mu)
uint64_t g_lus_top = 0;     // ... au top du script

void script_avancer() {   // sous g_mu
    if (!g_script_actif || !g_top_donne) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_top).count();
    const Gamepad* cur = nullptr;
    size_t k = 0;
    for (const Etape& e : g_script) { if (e.t <= t) { cur = &e.g; ++k; } else break; }
    static size_t dite = 0;
    if (k != dite) {
        dite = k;
        const Gamepad g = cur ? *cur : Gamepad{};
        fprintf(stderr, "[PAD] etape %zu/%zu a t=%.1f s : boutons=%04x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d ; "
                        "lectures idx0 depuis le top %llu\n",
                k, g_script.size(), t, g.boutons, g.lt, g.rt, g.lx, g.ly, g.rx, g.ry,
                (unsigned long long)(g_lus - g_lus_top));
    }
    publier(cur ? *cur : Gamepad{});
}

void initialiser() {
    static std::once_flag une;
    std::call_once(une, [] {
        g_file = dispatch_queue_create("bo2.pad", DISPATCH_QUEUE_SERIAL);
        if (const char* s = bo2_getenv("BO2_PAD_SCRIPT")) {
            charger_script(s);
            if (g_script_actif) { g_branchee = true; activer_gpad(); return; }
        }
        const char* e = bo2_getenv("BO2_PAD");
        if (e && atoi(e) == 0) { fprintf(stderr, "[PAD] manette reelle ETEINTE (BO2_PAD=0)\n"); return; }
        if (@available(macOS 11.3, *)) GCController.shouldMonitorBackgroundEvents = YES;
        NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
                    usingBlock:^(NSNotification* n) { brancher((GCController*)n.object); }];
        [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil
                    usingBlock:^(NSNotification* n) { debrancher((GCController*)n.object); }];
        for (GCController* c in GCController.controllers) brancher(c);
        fprintf(stderr, "[PAD] GameController ARME : %lu manette(s) deja la\n",
                (unsigned long)GCController.controllers.count);
    });
}

}  // namespace

// XInputGetState : 16 octets de XINPUT_STATE dans `out`. Rend ERROR_SUCCESS ou 0x48F.
extern "C" uint32_t bo2_pad_etat(uint32_t idx, uint8_t out[16]) {
    initialiser();
    std::lock_guard<std::mutex> lk(g_mu);
    script_avancer();
    if (idx == 0) {
        const uint64_t k = ++g_lus;
        if ((k & (k - 1)) == 0)
            fprintf(stderr, "[PAD] XInputGetState #%llu (idx 0, %s)\n", (unsigned long long)k,
                    g_branchee ? "branchee" : "absente");
    }
    if (idx != 0 || !g_branchee) return kNonBranchee;
    std::memcpy(out, &g_paquet, 4);
    std::memcpy(out + 4, &g_etat, 12);
    return kSucces;
}

// XInputGetCapabilities : 20 octets de XINPUT_CAPABILITIES (Type=GAMEPAD, SubType=GAMEPAD).
extern "C" uint32_t bo2_pad_capacites(uint32_t idx, uint8_t out[20]) {
    initialiser();
    std::lock_guard<std::mutex> lk(g_mu);
    if (idx != 0 || !g_branchee) return kNonBranchee;
    std::memset(out, 0, 20);
    out[0] = 1; out[1] = 1;                          // XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD
    const uint16_t tous = 0xF3FF;                    // tous les boutons existent
    std::memcpy(out + 4, &tous, 2);
    out[6] = out[7] = 0xFF;                          // gachettes analogiques
    const int16_t plein = 0x7FC0;                    // sticks analogiques (resolution annoncee)
    for (int k = 0; k < 4; k++) std::memcpy(out + 8 + 2 * k, &plein, 2);
    std::memset(out + 16, 0xFF, 4);                  // deux moteurs de vibration
    return kSucces;
}

// XInputSetState : la vibration. Pas encore rendue (CHHapticEngine) ; comptee pour le temoin.
extern "C" uint32_t bo2_pad_vibrer(uint32_t idx, uint16_t gauche, uint16_t droite) {
    initialiser();
    static uint64_t n = 0;
    const uint64_t k = (gauche || droite) ? ++n : 0;
    if (k && (k & (k - 1)) == 0)
        fprintf(stderr, "[PAD] vibration #%llu demandee (%u, %u), pas encore rendue\n",
                (unsigned long long)n, gauche, droite);
    std::lock_guard<std::mutex> lk(g_mu);
    return (idx == 0 && g_branchee) ? kSucces : kNonBranchee;
}

// Le top du script : appele par le runtime au [DB-LOADXFILE] de la carte mesuree.
extern "C" void bo2_pad_top() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_top_donne) return;
    g_top = std::chrono::steady_clock::now(); g_top_donne = true; g_lus_top = g_lus;
    if (g_script_actif) fprintf(stderr, "[PAD] top du script (%llu lectures idx0 avant)\n",
                                (unsigned long long)g_lus);
    if (g_branchee) activer_gpad();
}
