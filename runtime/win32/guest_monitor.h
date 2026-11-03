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
constexpr uint32_t kGuestMonitorW = 1920;
constexpr uint32_t kGuestMonitorH = 1080;
