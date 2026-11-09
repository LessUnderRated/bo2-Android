#pragma once
// 🎯 P657 — LE MONITEUR QUE VOIT L'INVITE : UN SEUL, POUR TOUTES LES API.
//
// Sous Windows, GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN), EnumDisplaySettings,
// GetMonitorInfo et IDXGIOutput::GetDesc / GetDisplayModeList decrivent LE MEME
// moniteur. Ici user32 renvoyait l'ecran reel du Mac (1680x1050 points, 16:10) et
// DXGI un 1920x1080 : le jeu prenait un aspect d'affichage de 1,6 et dessinait dans
// une fenetre 16:9 a pixels carres. Vu par le testeur : la video 1280x720 de l'intro
// couvrait toute la largeur et 648/720 de la hauteur, 0,9 = (16/10)/(16/9) — le
// dessous (menu, monde) visible dans les deux bandes.
//
// Le moniteur invite est celui que la presentation Metal reproduit : 16:9 a pixels
// carres (metal_backend.mm met l'image de l'invite a l'echelle UNIFORMEMENT, bandes
// en plein ecran 16:10), c'est-a-dire le seul mode que DXGI annonce.
#include <cstdint>
#if defined(__ANDROID__)
// Android : le moniteur suit le plan d'affichage du shell (Bo2-APK docs/DISPLAY.md) : la forme de l'ecran du
// telephone (19,5:9 sur un S26) ou 16:9, a la hauteur choisie. Le shell pose BO2_GUEST_W et BO2_GUEST_H avant que le
// jeu demarre ; lus une fois, puis les memes partout (la regle de P657 reste). Sans eux : 1920x1080, comme sur Mac.
#include <cstdlib>
inline uint32_t bo2_guest_monitor_dim(const char* name, uint32_t dflt) {
    const char* e = getenv(name);
    const unsigned long v = e ? strtoul(e, nullptr, 10) : 0;
    return v >= 320 && v <= 8192 ? (uint32_t)v : dflt;
}
inline uint32_t bo2_guest_monitor_w() { static const uint32_t w = bo2_guest_monitor_dim("BO2_GUEST_W", 1920); return w; }
inline uint32_t bo2_guest_monitor_h() { static const uint32_t h = bo2_guest_monitor_dim("BO2_GUEST_H", 1080); return h; }
#define kGuestMonitorW (bo2_guest_monitor_w())
#define kGuestMonitorH (bo2_guest_monitor_h())
#else
constexpr uint32_t kGuestMonitorW = 1920;
constexpr uint32_t kGuestMonitorH = 1080;
#endif
