// user32_impl.cpp — USER32 import implementations.
//
// For a menu to render, the game needs:
//   - Window creation (CreateWindowExA) → we return a fake window handle
//     (the real rendering goes through D3D11→Metal, not GDI)
//   - Message loop (GetMessage/PeekMessage/Dispatch/Translate) → stubbed
//     (returns WM_QUIT or no messages; wired to Cocoa events later)
//   - Display metrics (GetSystemMetrics, EnumDisplaySettings, GetMonitorInfo)
//     → the guest monitor of guest_monitor.h, the same one DXGI reports (P657)
//   - Input (GetCursorPos, SetCursorPos, MapVirtualKey) → CGEvent queries
//   - Window info (GetClientRect, GetWindowRect, AdjustWindowRect) → fake
//     (the game uses these to set up the D3D11 swap chain dimensions)
//
// The actual window/surface for Metal rendering is created separately by
// the D3D11→Metal layer, not by these USER32 stubs.
#include "win32_shim.h"
#include "bo2_getenv.h"
#include "guest_monitor.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#else
#include "bo2_platform.h"   // CLOCK_UPTIME_RAW, clock_gettime_nsec_np
// Android : la geometrie CoreGraphics dont ce fichier se sert, et le pointeur par le backend Android (SDL). La fenetre
// y couvre l'ecran, donc ses coordonnees sont celles de l'ecran, comme les points globaux de CG sur Mac.
struct CGPoint { double x, y; };
struct CGSize { double width, height; };
struct CGRect { CGPoint origin; CGSize size; };
static inline CGPoint CGPointMake(double x, double y) { return CGPoint{x, y}; }
static inline CGRect CGRectMake(double x, double y, double w, double h) { return CGRect{{x, y}, {w, h}}; }
typedef uint32_t CGDisplayCount;
typedef int CGError;
enum { kCGErrorSuccess = 0 };
static inline CGError CGGetActiveDisplayList(uint32_t, void*, CGDisplayCount* n) { if (n) *n = 1; return 0; }
extern "C" void gfx_cursor_location(double* x, double* y);   // backend Android : position du pointeur
extern "C" int gfx_cursor_warp(double x, double y);           // backend Android : 0 si le pointeur a ete deplace
static inline CGError CGWarpMouseCursorPosition(CGPoint p) { return gfx_cursor_warp(p.x, p.y) ? 1 : 0; }
static inline CGError CGAssociateMouseAndMouseCursorPosition(bool) { return kCGErrorSuccess; }
#endif

// Real window-server focus, published by metal_backend.mm unmasked by
// BO2_FORCE_FOCUS. The cursor recentring must obey this, never the forced lie.
extern "C" int gfx_window_is_really_key(void);
extern "C" int gfx_pointer_engaged(void);     // C16 : clic dans la fenêtre, rendu par ⌘
extern "C" int gfx_window_centre(double* x, double* y);    // milieu de la zone de jeu, points d'écran globaux ;
                                                           // 0 si la pompe ne l'a pas publié depuis 0,5 s
extern "C" void gfx_pointer_pinned(void);                  // le jeu vient de recentrer un pointeur tenu


extern "C" {
    uint32_t win32_mem_read32(uint32_t a);
    void win32_mem_write32(uint32_t a, uint32_t v);
    uint8_t win32_mem_read8(uint32_t a);
    void win32_mem_write8(uint32_t a, uint8_t v);
    void win32_mem_write16(uint32_t a, uint16_t v);
}

// --- fake handle counter (shared with kernel32) ---
// 🔴 P628/E8 — atomique depuis le 2026-09-21 : c'est un global, incremente
// aussi depuis des fils HOTES (voir sa definition dans kernel32_impl.cpp).
#include <atomic>
extern std::atomic<uint32_t> t_fake_handle_counter;
static uint32_t g_fake_window = 0;
// The one window. Exposed so the input pump can address messages to it.
extern "C" uint32_t win32_guest_hwnd() { return g_fake_window; }
static uint32_t g_fake_dc = 0;

// --- window dimensions (the game sets these, we store them) ---
static int g_win_x = 0, g_win_y = 0, g_win_w = 1280, g_win_h = 720;

// --- DISPLAY METRICS (real CoreGraphics queries) ---

static void impl_GetSystemMetrics(State& s) {
    // GetSystemMetrics(index) — stdcall 1 arg
    uint32_t index = win32_arg(s, 0);
    uint32_t result = 0;
    switch (index) {
        case 0:  // SM_CXSCREEN
            result = kGuestMonitorW;
            break;
        case 1:  // SM_CYSCREEN
            result = kGuestMonitorH;
            break;
        case 2:  // SM_CXVSCROLL
            result = 17; break;
        case 3:  // SM_CYHSCROLL
            result = 17; break;
        case 4:  // SM_CYCAPTION
            result = 22; break;
        case 5:  // SM_CXBORDER
            result = 1; break;
        case 6:  // SM_CYBORDER
            result = 1; break;
        case 7:  // SM_CXDLGFRAME
            result = 3; break;
        case 8:  // SM_CYDLGFRAME
            result = 3; break;
        case 12: // SM_CXFULLSCREEN
            result = kGuestMonitorW; break;
        case 13: // SM_CYFULLSCREEN
            result = kGuestMonitorH; break;
        case 15: // SM_CYMENU
            result = 20; break;
        case 16: // SM_CXFRAME
            result = 4; break;
        case 17: // SM_CYFRAME
            result = 4; break;
        case 19: // SM_CXMINTRACK
            result = 136; break;
        case 20: // SM_CYMINTRACK
            result = 39; break;
        case 21: // SM_CXDOUBLECLK
            result = 4; break;
        case 22: // SM_CYDOUBLECLK
            result = 4; break;
        case 23: // SM_CXICONSPACING
            result = 75; break;
        case 24: // SM_CYICONSPACING
            result = 75; break;
        case 28: // SM_CXMINIMIZED
            result = 160; break;
        case 29: // SM_CYMINIMIZED
            result = 28; break;
        case 30: // SM_CXMAXIMIZED
            result = kGuestMonitorW + 8; break;
        case 31: // SM_CYMAXIMIZED
            result = kGuestMonitorH + 8; break;
        case 32: // SM_NETWORK
            result = 3; break; // network present
        case 38: // SM_CMONITORS (Windows 98+)
            { CGDisplayCount count = 0; CGGetActiveDisplayList(0, nullptr, &count); result = count ? count : 1; }
            break;
        case 40: // SM_CXMAXTRACK
            result = 0x7FFFFFFF; break;
        case 41: // SM_CYMAXTRACK
            result = 0x7FFFFFFF; break;
        case 42: // SM_CXMENUCHECK
            result = 19; break;
        case 43: // SM_CYMENUCHECK
            result = 19; break;
        case 44: // SM_SLOWMACHINE
            result = 0; break;
        case 45: // SM_MIDEASTENABLED
            result = 0; break;
        case 67: // SM_CXFIXEDFRAME (same as SM_CXDLGFRAME)
            result = 3; break;
        case 68: // SM_CYFIXEDFRAME
            result = 3; break;
        case 78: // SM_CXSMICON
            result = 16; break;
        case 79: // SM_CYSMICON
            result = 16; break;
        case 80: // SM_CYSMCAPTION
            result = 18; break;
        case 83: // SM_CXSMICON (same as 78)
            result = 16; break;
        default:
            result = 0;
            fprintf(stderr, "[user32] GetSystemMetrics(%u) unhandled\n", index);
    }
    s.gpr.rax.dword = result;
    win32_stdcall_ret(s, 1);
}

static void impl_EnumDisplaySettingsA(State& s) {
    // EnumDisplaySettingsA(device_name, mode_num, @devmode) — stdcall 3 args
    uint32_t mode_num = win32_arg(s, 1);
    uint32_t devmode_ptr = win32_arg(s, 2);
    if (mode_num == 0) {  // current mode
        // DEVMODE structure (simplified): key fields at known offsets
        // dmSize at offset 0 (WORD), dmFields at some offset
        // dmPelsWidth at offset 104 (DWORD) — varies by Windows version
        // This is a simplified layout matching Win32 DEVMODEA
        if (devmode_ptr) {
            win32_mem_write16(devmode_ptr + 0, 220);  // dmSize
            win32_mem_write32(devmode_ptr + 104, kGuestMonitorW);
            win32_mem_write32(devmode_ptr + 108, kGuestMonitorH);
            win32_mem_write32(devmode_ptr + 112, 32);  // dmBitsPerPel
            win32_mem_write32(devmode_ptr + 116, 60);  // dmDisplayFrequency
        }
        s.gpr.rax.dword = 1; // success
    } else if (mode_num == 0xFFFFFFFF) {  // ENUM_REGISTRY_SETTINGS
        s.gpr.rax.dword = 0;
    } else {
        s.gpr.rax.dword = 0; // no more modes
    }
    win32_stdcall_ret(s, 3);
}

static void impl_ChangeDisplaySettingsA(State& s) {
    // ChangeDisplaySettingsA(@devmode, flags) — stdcall 2 args
    // We don't actually change display settings on macOS.
    // Return 0 (DISP_CHANGE_SUCCESSFUL)
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 2);
}

static void impl_GetMonitorInfoA(State& s) {
    // GetMonitorInfoA(hmonitor, @mi) — stdcall 2 args
    uint32_t mi_ptr = win32_arg(s, 1);
    const CGRect bounds = CGRectMake(0, 0, kGuestMonitorW, kGuestMonitorH);   // P657
    if (mi_ptr) {
        // MONITORINFO: cbSize(4), rcMonitor(16), rcWork(16), dwFlags(4) = 40 bytes
        win32_mem_write32(mi_ptr, 40);  // cbSize
        // rcMonitor: left, top, right, bottom
        win32_mem_write32(mi_ptr + 4, (uint32_t)bounds.origin.x);
        win32_mem_write32(mi_ptr + 8, (uint32_t)bounds.origin.y);
        win32_mem_write32(mi_ptr + 12, (uint32_t)(bounds.origin.x + bounds.size.width));
        win32_mem_write32(mi_ptr + 16, (uint32_t)(bounds.origin.y + bounds.size.height));
        // rcWork: same as rcMonitor for primary display
        win32_mem_write32(mi_ptr + 20, (uint32_t)bounds.origin.x);
        win32_mem_write32(mi_ptr + 24, (uint32_t)bounds.origin.y);
        win32_mem_write32(mi_ptr + 28, (uint32_t)(bounds.origin.x + bounds.size.width));
        win32_mem_write32(mi_ptr + 32, (uint32_t)(bounds.origin.y + bounds.size.height));
        // dwFlags: MONITORINFOF_PRIMARY = 1
        win32_mem_write32(mi_ptr + 36, 1);
    }
    s.gpr.rax.dword = 1; // success
    win32_stdcall_ret(s, 2);
}

static void impl_MonitorFromWindow(State& s) {
    // MonitorFromWindow(hwnd, flags) — stdcall 2 args
    // Return a fake monitor handle (1 = primary)
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}

// --- WINDOW CREATION (stubs — real window is Metal layer) ---

// Both class registrations record lpfnWndProc so window messages can be
// delivered to the guest's own procedure. WNDCLASSA has it at +4 (style,
// lpfnWndProc, ...); WNDCLASSEXA has a cbSize first, so it is at +8. Reading it
// beats hardcoding the address the disassembly shows (0x4a63d0): a patched or
// differently-built binary registers a different one, and a stale constant here
// would silently deliver every message to the wrong function.
static void impl_RegisterClassA(State& s) {
    uint32_t wc = win32_arg(s, 0);
    if (wc) win32_set_wndproc(win32_mem_read32(wc + 4));
    s.gpr.rax.dword = 0xC000;  // fake atom
    win32_stdcall_ret(s, 1);
}

static void impl_RegisterClassExA(State& s) {
    uint32_t wc = win32_arg(s, 0);
    if (wc) win32_set_wndproc(win32_mem_read32(wc + 8));
    s.gpr.rax.dword = 0xC000;
    win32_stdcall_ret(s, 1);
}

static void impl_RegisterWindowMessageA(State& s) {
    s.gpr.rax.dword = 0xC001;
    win32_stdcall_ret(s, 1);
}

static void impl_CreateWindowExA(State& s) {
    // CreateWindowExA(ex_style, class, name, style, x, y, w, h, parent, menu, instance, param) — stdcall 12 args
    uint32_t x = win32_arg(s, 4);
    uint32_t y = win32_arg(s, 5);
    uint32_t w = win32_arg(s, 6);
    uint32_t h = win32_arg(s, 7);
    if (w != 0xC0000000) g_win_w = (int)w;  // CW_USEDEFAULT = 0x80000000
    if (h != 0xC0000000) g_win_h = (int)h;
    g_win_x = (int)x;
    g_win_y = (int)y;
    if (g_fake_window == 0) g_fake_window = ++t_fake_handle_counter;
    s.gpr.rax.dword = g_fake_window;
    win32_stdcall_ret(s, 12);
    // WM_CREATE, exactly where the real CreateWindowEx sends it: before it
    // returns. THIS IS NOT COSMETIC — the engine's WNDPROC case for message 1
    // is the ONLY writer of its own HWND global (0x2b5a4d8), and every later
    // input decision compares GetForegroundWindow() against that global. With
    // no WM_CREATE it stayed 0 and the whole input path was dead. Sent after
    // the stdcall cleanup so the WNDPROC runs on a clean guest stack.
    win32_send_message(&s, g_fake_window, 0x0001, 0, 0);
}

static void impl_ShowWindow(State& s) {
    // ShowWindow(hwnd, cmd_show) — stdcall 2 args
    s.gpr.rax.dword = 1; // was visible
    win32_stdcall_ret(s, 2);
}

static void impl_DestroyWindow(State& s) {
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 1);
}

static void impl_CloseWindow(State& s) {
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 1);
}

static void impl_MoveWindow(State& s) {
    // MoveWindow(hwnd, x, y, w, h, repaint) — stdcall 6 args
    g_win_x = (int)win32_arg(s, 1);
    g_win_y = (int)win32_arg(s, 2);
    g_win_w = (int)win32_arg(s, 3);
    g_win_h = (int)win32_arg(s, 4);
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 6);
}

static void impl_SetWindowPos(State& s) {
    // SetWindowPos(hwnd, after, x, y, cx, cy, flags) — stdcall 7 args
    g_win_x = (int)win32_arg(s, 2);
    g_win_y = (int)win32_arg(s, 3);
    g_win_w = (int)win32_arg(s, 4);
    g_win_h = (int)win32_arg(s, 5);
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 7);
}

static void impl_SetWindowTextA(State& s) {
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}

static void impl_GetWindowTextA(State& s) {
    // GetWindowTextA(hwnd, buf, max_count) — stdcall 3 args
    uint32_t buf = win32_arg(s, 1);
    if (buf) win32_mem_write8(buf, 0); // empty string
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 3);
}

// --- WINDOW INFO ---

static void impl_GetClientRect(State& s) {
    // GetClientRect(hwnd, @rect) — stdcall 2 args
    uint32_t rect_ptr = win32_arg(s, 1);
    if (rect_ptr) {
        win32_mem_write32(rect_ptr, 0);           // left
        win32_mem_write32(rect_ptr + 4, 0);       // top
        win32_mem_write32(rect_ptr + 8, g_win_w); // right
        win32_mem_write32(rect_ptr + 12, g_win_h);// bottom
    }
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}

static void impl_GetWindowRect(State& s) {
    uint32_t rect_ptr = win32_arg(s, 1);
    if (rect_ptr) {
        win32_mem_write32(rect_ptr, g_win_x);
        win32_mem_write32(rect_ptr + 4, g_win_y);
        win32_mem_write32(rect_ptr + 8, g_win_x + g_win_w);
        win32_mem_write32(rect_ptr + 12, g_win_y + g_win_h);
    }
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}

static void impl_AdjustWindowRect(State& s) {
    // AdjustWindowRect(@rect, style, menu) — stdcall 3 args
    // Just return the rect unchanged (no decoration adjustments)
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 3);
}

static void impl_AdjustWindowRectEx(State& s) {
    // AdjustWindowRectEx(@rect, style, menu, ex_style) — stdcall 4 args
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 4);
}

static void impl_ClientToScreen(State& s) {
    // ClientToScreen(hwnd, @pt) — stdcall 2 args
    uint32_t pt_ptr = win32_arg(s, 1);
    if (pt_ptr) {
        // x += g_win_x, y += g_win_y
        uint32_t x = win32_mem_read32(pt_ptr);
        uint32_t y = win32_mem_read32(pt_ptr + 4);
        win32_mem_write32(pt_ptr, x + g_win_x);
        win32_mem_write32(pt_ptr + 4, y + g_win_y);
    }
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}

static void impl_ScreenToClient(State& s) {
    uint32_t pt_ptr = win32_arg(s, 1);
    if (pt_ptr) {
        uint32_t x = win32_mem_read32(pt_ptr);
        uint32_t y = win32_mem_read32(pt_ptr + 4);
        win32_mem_write32(pt_ptr, x - g_win_x);
        win32_mem_write32(pt_ptr + 4, y - g_win_y);
    }
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}

// --- MESSAGE LOOP ---
// t6mp has the same pump; with the stubs below its menu took no key, real or injected (2026-10-20, [WNDPROC] saw
// WM_CREATE only).
// The client's pump is the game's own: its frame function calls Present, then (0x74846e-0x7484df)
//   while (PeekMessageA(&msg,0,0,0,PM_NOREMOVE)) { GetMessageA(&msg,0,0,0); [0x2b3f0a0] = msg.time;
//                                                  TranslateMessage(&msg); DispatchMessageA(&msg); }
// (the two classic pumps, 0x4c65d7 and 0x8d7624, are behind the `dedicated` dvar). With these four answering "no
// message" the WNDPROC never saw a key: Enter on ONLINE did nothing (run of 08/10, Z-NATIF-09g).
// The queue is the host one metal_backend.mm fills (window events, BO2_KEYPIPE / BO2_CLICKPIPE); WM_CHAR is already
// in it, so TranslateMessage has nothing to post. t6sp keeps its pump on the input frame (win32_runtime.cpp).
extern "C" bool bo2_journal_input_pop(uint32_t* msg, uint32_t* wp, uint32_t* lp);
extern "C" uint64_t bo2_mono_ns();
extern "C" int win32_current_tid();
extern "C" void bo2_cmd_drain(State* s);            // BO2_CMDPIPE -> the game's Cbuf_AddText (win32_runtime.cpp)
namespace {
struct { uint32_t msg, wp, lp; bool have; } g_front;   // head of the queue; the pumping guest thread only
uint32_t g_msg_pt[2];                                  // cursor, screen coordinates, at the last mouse message

bool msg_front() {
    if (!g_front.have) g_front.have = bo2_journal_input_pop(&g_front.msg, &g_front.wp, &g_front.lp);
    return g_front.have;
}
// MSG { hwnd, message, wParam, lParam, time, pt }, then the message leaves the queue if asked.
void msg_take(uint32_t p, bool remove) {
    if (g_front.msg >= 0x0200u && g_front.msg <= 0x020eu) {       // WM_MOUSEFIRST..WM_MOUSELAST: lParam = client x, y
        g_msg_pt[0] = (uint32_t)(g_win_x + (int16_t)(g_front.lp & 0xffffu));
        g_msg_pt[1] = (uint32_t)(g_win_y + (int16_t)(g_front.lp >> 16));
    }
    win32_mem_write32(p, g_fake_window);
    win32_mem_write32(p + 4, g_front.msg);
    win32_mem_write32(p + 8, g_front.wp);
    win32_mem_write32(p + 12, g_front.lp);
    win32_mem_write32(p + 16, (uint32_t)(bo2_mono_ns() / 1000000ull));   // the clock of GetTickCount
    win32_mem_write32(p + 20, g_msg_pt[0]);
    win32_mem_write32(p + 24, g_msg_pt[1]);
    if (remove) g_front.have = false;
}
}  // namespace

static void impl_PeekMessageA(State& s) {
    // PeekMessageA(@msg, hwnd, min, max, remove) — stdcall 5 args
    static bool said = false;
    if (!said) {
        said = true;
        fprintf(stderr, "[POMPE] premier PeekMessageA tid=%d ret=0x%08x\n", win32_current_tid(),
                win32_mem_read32(s.gpr.rsp.dword));
    }
    bo2_cmd_drain(&s);
    const bool have = msg_front();
    if (have) msg_take(win32_arg(s, 0), win32_arg(s, 4) & 1u /* PM_REMOVE */);
    s.gpr.rax.dword = have;
    win32_stdcall_ret(s, 5);
}

static void impl_GetMessageA(State& s) {
    // GetMessageA(@msg, hwnd, min, max) — stdcall 4 args; 0 for WM_QUIT only.
    // ponytail: on an empty queue Windows blocks; this answers WM_QUIT as the stub did. The pumps call it behind a
    // PeekMessageA that answered 1; only the fatal-error loop (0x6cbc09) waits here, and it then exits at once
    // instead of waiting for its window to be closed. Block (yielding to the scheduler) if another caller appears.
    const bool have = msg_front();
    const uint32_t msg = g_front.msg;
    if (have) msg_take(win32_arg(s, 0), true);
    s.gpr.rax.dword = have && msg != 0x0012u;
    win32_stdcall_ret(s, 4);
}

static void impl_TranslateMessage(State& s) {
    const uint32_t msg = win32_mem_read32(win32_arg(s, 0) + 4);
    s.gpr.rax.dword = (msg >= 0x0100u && msg <= 0x0105u && msg != 0x0102u && msg != 0x0103u);   // a key message
    win32_stdcall_ret(s, 1);
}

static void impl_DispatchMessageA(State& s) {
    // DispatchMessageA(@msg) — the window procedure's own result. One window, one class.
    const uint32_t p = win32_arg(s, 0);
    const uint32_t hwnd = win32_mem_read32(p), msg = win32_mem_read32(p + 4), wp = win32_mem_read32(p + 8),
                   lp = win32_mem_read32(p + 12);
    win32_stdcall_ret(s, 1);                       // the WNDPROC runs on a clean guest stack, as for WM_CREATE
    s.gpr.rax.dword = win32_send_message(&s, hwnd, msg, wp, lp);
}

static void impl_PostQuitMessage(State& s) {
    // PostQuitMessage(exit_code) — void, stdcall 1 arg
    win32_stdcall_ret(s, 1);
}

static void impl_PostMessageA(State& s) {
    // PostMessageA(hwnd, msg, wparam, lparam) — stdcall 4 args
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 4);
}

static void impl_SendMessageA(State& s) {
    // SendMessageA(hwnd, msg, wparam, lparam) — stdcall 4 args
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 4);
}

static void impl_DefWindowProcA(State& s) {
    // DefWindowProcA(hwnd, msg, wparam, lparam) — stdcall 4 args
    s.gpr.rax.dword = 0; // DefWindowProc returns 0 for most messages
    win32_stdcall_ret(s, 4);
}

static void impl_CallWindowProcA(State& s) {
    // CallWindowProcA(proc, hwnd, msg, wparam, lparam) — stdcall 5 args
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 5);
}

// --- INPUT ---

// ── VIRTUAL CURSOR — the camera gets its deltas, the user keeps their mouse ──
//
// A mouse-look camera does: read cursor, delta = cursor - centre, rotate, then
// SetCursorPos back to centre so the next frame starts from zero.
//
// The first implementation of SetCursorPos warped the REAL macOS cursor. It made
// the camera behave — and pinned the tester's pointer for his whole machine, because
// the game recentres EVERY FRAME. "ça marche mais je veux pas que le curseur soit
// bloqué" is the correct complaint: a port should not seize the system pointer.
//
// So keep the accounting instead of the pointer. We track where the real cursor
// has moved SINCE the guest last recentred, and hand the guest
// virtual = centre_it_asked_for + (real_now - real_at_that_moment). The guest
// sees exactly the deltas it expects, whether or not the physical cursor moves.
//
// Since C16 the pointer is held only after a click in the game area and given back by ⌘, so while it is
// held SetCursorPos does what it does under Windows: it puts the pointer back (impl_SetCursorPos).
// Otherwise the physical cursor is never touched.
static double g_virt_x = 0, g_virt_y = 0;      // what the guest is told
static double g_real_ref_x = 0, g_real_ref_y = 0;  // real cursor at the last recentre
static bool   g_cursor_init = false;
// Mouse look or menu? In mouse look the game answers every read that moved by putting the pointer back; in a menu
// it never does. Three moved reads in a row left unanswered = menu, and the pointer is shown again. A pointer at
// rest says nothing, so in mouse look it stays hidden (le testeur, 2026-10-10 : « ni vu ni connu »).
static bool   g_visee = false, g_lu_bouge = false;
static int    g_sans_reponse = 0;

static void cursor_real(double& x, double& y) {
#if defined(__APPLE__)
    CGEventRef e = CGEventCreate(nullptr);
    CGPoint loc = CGEventGetLocation(e);
    CFRelease(e);
    x = loc.x; y = loc.y;
#else
    gfx_cursor_location(&x, &y);
#endif
}

// Témoin du pointeur tenu (le testeur, 2026-10-10 : « invisible, mais il bloque toujours aux bords de l'écran »). Il ne
// change rien. [CURSOR-LOIN] sort quand un pointeur tenu est lu à plus de 300 points du centre, avec de quoi séparer
// « le jeu ne recentre pas » (dernier SetCursorPos ancien), « recentrage refusé » (refus != 0 : bit 0 fenêtre pas au
// premier plan, 1 pas de clic, 2 BO2_CURSOR_GRAB=0, 3 centre périmé) et « recentrage sans effet » (recentrage récent).
static struct {
    uint64_t get, set, tenus, warps, err, loin, perimes;
    double dist_max, t_set, t_warp, t_ligne, t_bilan;
    unsigned refus;
} g_ct;
static double ct_now() { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9; }
static void ct_get(double rx, double ry) {
    static bool arme = false;
    if (!arme) {
        arme = true;
        fprintf(stderr, "[CURSOR-TEMOIN] arme : [CURSOR-10S] toutes les 10 s de pointeur tenu, [CURSOR-LOIN] au plus "
                        "une fois par seconde quand il est lu a plus de 300 points du centre\n");
    }
    if (!gfx_window_is_really_key() || !gfx_pointer_engaged()) return;
    const double t = ct_now();
    g_ct.get++;
    double cx = 0, cy = 0;
    if (!gfx_window_centre(&cx, &cy)) {
        g_ct.perimes++;
    } else {
        const double d = fmax(fabs(rx - cx), fabs(ry - cy));
        if (d > g_ct.dist_max) g_ct.dist_max = d;
        if (d > 300.0) {
            g_ct.loin++;
            if (t - g_ct.t_ligne >= 1.0) {
                g_ct.t_ligne = t;
                fprintf(stderr, "[CURSOR-LOIN] pointeur tenu lu a %.0f points du centre (reel %.0f, %.0f ; centre %.0f, %.0f) : "
                                "dernier SetCursorPos il y a %.0f ms (refus 0x%x), dernier recentrage il y a %.0f ms, "
                                "erreurs CGWarp %llu\n", d, rx, ry, cx, cy, (t - g_ct.t_set) * 1e3, g_ct.refus,
                        (t - g_ct.t_warp) * 1e3, (unsigned long long)g_ct.err);
            }
        }
    }
    if (t - g_ct.t_bilan >= 10.0) {
        fprintf(stderr, "[CURSOR-10S] GetCursorPos %llu (centre perime %llu), SetCursorPos %llu dont tenus %llu, "
                        "recentrages %llu, erreurs CGWarp %llu, lectures a plus de 300 points %llu, distance max %.0f\n",
                (unsigned long long)g_ct.get, (unsigned long long)g_ct.perimes, (unsigned long long)g_ct.set,
                (unsigned long long)g_ct.tenus, (unsigned long long)g_ct.warps, (unsigned long long)g_ct.err,
                (unsigned long long)g_ct.loin, g_ct.dist_max);
        g_ct.get = g_ct.perimes = g_ct.set = g_ct.tenus = g_ct.warps = g_ct.loin = 0;
        g_ct.dist_max = 0;
        g_ct.t_bilan = t;
    }
}

static void impl_GetCursorPos(State& s) {
    // GetCursorPos(@point) — stdcall 1 arg
    uint32_t pt_ptr = win32_arg(s, 0);
    g_n_cursor_poll++;
    if (pt_ptr) {
        double rx, ry; cursor_real(rx, ry);
        ct_get(rx, ry);
        if (!g_cursor_init) {
            g_cursor_init = true;
            g_virt_x = rx; g_virt_y = ry;
            g_real_ref_x = rx; g_real_ref_y = ry;
        }
        // BO2_FORCE_FOCUS fait croire à l'invité qu'il est au premier plan ; sous Windows,
        // un jeu en arrière-plan ne voit pas la souris des autres applications. Le testeur,
        // 2026-09-26 : « quand je suis sur d'autres logiciels, le curseur fait tourner la
        // caméra ». Hors fenêtre clé, on réancre : l'invité lit un delta nul.
        if (!gfx_window_is_really_key()) { g_real_ref_x = rx; g_real_ref_y = ry; }
        // The delta is rounded, not truncated: a pointer read back a hair short of where it was put would
        // otherwise turn the camera by one count every frame.
        const double vx = g_virt_x + (double)lround(rx - g_real_ref_x);
        const double vy = g_virt_y + (double)lround(ry - g_real_ref_y);
        win32_mem_write32(pt_ptr,     (uint32_t)(int32_t)vx);
        win32_mem_write32(pt_ptr + 4, (uint32_t)(int32_t)vy);
        if (g_lu_bouge && ++g_sans_reponse >= 3) g_visee = false;
        g_lu_bouge = vx != g_virt_x || vy != g_virt_y;
        if (g_visee) gfx_pointer_pinned();
    }
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 1);
}

static void impl_SetCursorPos(State& s) {
    // SetCursorPos(x, y) — stdcall 2 args
    //
    // The guest is telling us where it believes the cursor now is (its recentre).
    // Record that as the virtual position and re-anchor to wherever the real
    // pointer currently is, so the next GetCursorPos reports a delta of zero and
    // the camera stops drifting — WITHOUT moving the user's mouse.
    const int32_t x = (int32_t)win32_arg(s, 0);
    const int32_t y = (int32_t)win32_arg(s, 1);
    g_virt_x = x; g_virt_y = y;

    // WHILE THE POINTER IS HELD (C16: after a click in the game area, until ⌘ or the loss of the front), it is
    // hidden and put back in the middle of the game area once it has wandered 220 points from it. Putting it
    // back on every call (2026-10-10 01:49) left the tester with a pointer that « bloque toujours aux bords de
    // l'écran »; the 220 points are what he played with before, and asked for again the same day.
    // BO2_CURSOR_DRIFT=<points> changes the distance (0 = put back on every call).
    double rx, ry; cursor_real(rx, ry);
    if (!g_cursor_init) { g_real_ref_x = rx; g_real_ref_y = ry; }
    double cx, cy; const bool centre_frais = gfx_window_centre(&cx, &cy);
    const double drift = fabs(rx - cx) + fabs(ry - cy);
    static const double kDrift = [] {
        const char* e = bo2_getenv("BO2_CURSOR_DRIFT");
        return e ? atof(e) : 220.0;      // points away from the centre before the pointer is put back
    }();
    // ⚠️ NEVER HOLD THE POINTER WHILE THE WINDOW IS NOT REALLY FOCUSED.
    // The tester had to kill the game TWICE to get his mouse back: the recentring
    // fires on a timer-ish drift threshold, so clicking away to another app just
    // yanked the pointer back into the game. BO2_FORCE_FOCUS tells the GUEST it
    // is foreground — that lie is for activation messages only, and must not
    // reach the user's physical cursor. gfx_window_is_really_key() reports the
    // window server's truth, unmasked.
    //
    // A grab the user cannot escape is a bug even when the rendering is perfect.
    //
    // 🔴 C16 : et jamais avant un clic DANS la fenêtre (gfx_pointer_engaged, rendu par ⌘
    // ou la perte du premier plan). Premier plan ne veut pas dire « on joue » : un run de
    // mesure ouvre sa fenêtre devant, et le recentrage a encore bloqué le pointeur du testeur
    // pour tout le Mac. BO2_CURSOR_GRAB=0 (lanceur de mesure) : ne jamais le tenir.
    static const bool grab_ok = [] {
        const char* e = bo2_getenv("BO2_CURSOR_GRAB");
        return !(e && e[0] == '0');
    }();
    g_ct.set++;
    g_ct.t_set = ct_now();
    g_ct.refus = (gfx_window_is_really_key() ? 0u : 1u) | (gfx_pointer_engaged() ? 0u : 2u) | (grab_ok ? 0u : 4u) |
                 (centre_frais ? 0u : 8u);
    if (!gfx_window_is_really_key() || !gfx_pointer_engaged() || !grab_ok || !centre_frais) {
        g_real_ref_x = rx; g_real_ref_y = ry;
        g_cursor_init = true;
        s.gpr.rax.dword = 1;
        win32_stdcall_ret(s, 2);
        return;
    }
    gfx_pointer_pinned();
    g_visee = true; g_lu_bouge = false; g_sans_reponse = 0;
    g_ct.tenus++;
    if (drift > kDrift) {
        if (CGWarpMouseCursorPosition(CGPointMake(cx, cy)) != kCGErrorSuccess) g_ct.err++;
        g_ct.warps++;
        g_ct.t_warp = g_ct.t_set;
        CGAssociateMouseAndMouseCursorPosition(true);   // or the window server holds the mouse still for 0.25 s
        g_real_ref_x = cx; g_real_ref_y = cy;
        static uint64_t nw = 0;
        if (++nw <= 3)
            fprintf(stderr, "[CURSOR] pointeur remis au centre de la zone de jeu (%.0f, %.0f), ecart %.3f "
                            "(seuil %.0f, BO2_CURSOR_DRIFT)\n", cx, cy, drift, kDrift);
    } else {
        g_real_ref_x = rx; g_real_ref_y = ry;
    }
    g_cursor_init = true;
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}

static void impl_SetCursor(State& s) {
    static uint64_t nuls = 0, autres = 0;   // witness only: does the game hide with SetCursor(NULL) or ShowCursor?
    const uint64_t n = win32_arg(s, 0) ? ++autres : ++nuls;
    if ((n & (n - 1)) == 0)
        fprintf(stderr, "[CURSOR] SetCursor : NULL=%llu autre=%llu\n", (unsigned long long)nuls, (unsigned long long)autres);
    s.gpr.rax.dword = 0; // previous cursor (fake)
    win32_stdcall_ret(s, 1);
}

static void impl_ShowCursor(State& s) {
    // ShowCursor(show) — returns counter, stdcall 1 arg. Under Windows the pointer shows while the counter
    // is >= 0. Not honoured yet: the count flips between 0 and -1 at frame rate in the menus, which no
    // Windows player sees as a blinking pointer, so something upstream of this call differs. Witness only.
    static int cursor_count = 0;
    int show = (int)win32_arg(s, 0);
    const int avant = cursor_count;
    cursor_count += show ? 1 : -1;
    static uint64_t nt = 0;
    if ((avant >= 0) != (cursor_count >= 0) && (++nt <= 4 || (nt & (nt - 1)) == 0))
        fprintf(stderr, "[CURSOR] ShowCursor(%d) : compteur %d -> %d, bascule #%llu\n", show, avant, cursor_count,
                (unsigned long long)nt);
    s.gpr.rax.dword = (uint32_t)cursor_count;
    win32_stdcall_ret(s, 1);
}

static void impl_MapVirtualKeyA(State& s) {
    // MapVirtualKeyA(code, type) — stdcall 2 args
    // TODO: proper key mapping. For now return 0.
    s.gpr.rax.dword = 0;
    win32_stdcall_ret(s, 2);
}

// --- DEVICE CONTEXT (stubs — Metal layer handles rendering) ---

static void impl_GetDC(State& s) {
    if (g_fake_dc == 0) g_fake_dc = ++t_fake_handle_counter;
    s.gpr.rax.dword = g_fake_dc;
    win32_stdcall_ret(s, 1);
}

static void impl_ReleaseDC(State& s) {
    s.gpr.rax.dword = 1;
    win32_stdcall_ret(s, 2);
}

// --- MISC STUBS ---

#define USTUB0(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 0); }
#define USTUB1(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 1); }
#define USTUB2(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 2); }
#define USTUB3(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 3); }
#define USTUB4(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 4); }
#define USTUB5(name) static void impl_##name(State& s) { s.gpr.rax.dword = 0; win32_stdcall_ret(s, 5); }
#define USTUB_HANDLE(name, n) static void impl_##name(State& s) { s.gpr.rax.dword = ++t_fake_handle_counter; win32_stdcall_ret(s, n); }

USTUB_HANDLE(LoadCursorA, 2)
USTUB_HANDLE(LoadImageA, 6)
USTUB_HANDLE(LoadIconA, 2)
// THE GAME'S OWN WINDOW, not a fresh handle. These used to be USTUB_HANDLE, so
// every call returned a DIFFERENT number — and the input frame (FUN_005fc160)
// decides whether to poll input by testing `GetForegroundWindow() == <its own
// hwnd>`. That test could never be true, so the whole input path was skipped
// and neither the controller poll (FUN_0083f410 -> XInputGetState) nor the
// mouse read (FUN_0083f680 -> GetCursorPos) ever ran. Same class as the D3D
// view bug: any place the shim hands back a NEW object is a place the guest
// cannot tell two objects apart. We have exactly one window, so say so.
static void impl_GetForegroundWindow(State& s) {
    s.gpr.rax.dword = g_fake_window;
    win32_stdcall_ret(s, 0);
}
static void impl_GetActiveWindow(State& s) {
    s.gpr.rax.dword = g_fake_window;
    win32_stdcall_ret(s, 0);
}
USTUB_HANDLE(GetDesktopWindow, 0)
USTUB_HANDLE(GetProcessWindowStation, 0)
USTUB_HANDLE(GetSystemMenu, 2)
USTUB1(SetFocus)
USTUB1(IsWindow)
// Stdcall arities MUST match the real signatures — a wrong count displaces the
// caller's stack by 4×delta and the corruption surfaces frames later (the
// USTUB1 Get/SetClassLongA here left GCL_STYLE=-0x1a on 0x6bc510's stack; its
// exit `pop ebp` then fed -0x1a to FUN_006bce50 whose `mov esp,ebp` epilogue
// wrecked ESP right after ShowWindow — a 3-session hunt).
USTUB2(GetWindowLongA)     // (hWnd, nIndex)
USTUB3(SetWindowLongA)     // (hWnd, nIndex, dwNewLong)
USTUB2(GetClassLongA)      // (hWnd, nIndex)
USTUB3(SetClassLongA)      // (hWnd, nIndex, dwNewLong)
USTUB1(OpenClipboard)      // (hWndNewOwner)
USTUB0(CloseClipboard)     // ()
USTUB1(GetClipboardData)   // (uFormat)
static void impl_MessageBoxA(State& s) {
    // Log the message — the game pops an error dialog then tears down; the text
    // names the exact init failure (Steam? missing file? subsystem?).
    auto dump = [&](const char* label, uint32_t p) {
        char buf[512]; int i = 0;
        for (; i < 511; i++) { buf[i] = (char)win32_mem_read8(p + i); if (!buf[i]) break; }
        buf[i] = 0;
        fprintf(stderr, "[MessageBoxA] %s: \"%s\"\n", label, buf);
    };
    dump("text", win32_arg(s, 1));
    dump("caption", win32_arg(s, 2));
    // Walk the GUEST stack to name the caller chain: this is the engine's
    // Com_ShowFatalError equivalent — the ret addresses naming the demand
    // chain back to the frame that decided to fatal.
    {
        uint32_t esp = s.gpr.rsp.dword;
        // skip past MessageBoxA's own retaddr (esp[0]) — caller of Com_ShowFatalError starts above
        fprintf(stderr, "[MessageBoxA-stack] esp=0x%08x ret0=0x%08x\n", esp, win32_mem_read32(esp));
        int shown = 0;
        for (uint32_t ad = esp + 0x10; ad < esp + 0x400 && shown < 24; ad += 4) {
            uint32_t rv = win32_mem_read32(ad);
            if (rv >= 0x401000u && rv < 0xa314b0u) {
                fprintf(stderr, "  [mb-stk %08x] ret=0x%08x args:[%08x %08x %08x %08x]\n",
                        ad, rv, win32_mem_read32(ad+4), win32_mem_read32(ad+8),
                        win32_mem_read32(ad+0xc), win32_mem_read32(ad+0x10));
                shown++;
            }
        }
        // Dump ASCII runs in the stack bytes — the game builds the error text
        // inline (e.g. "Couldn't get...") so it won't show in .rdata scans.
        fprintf(stderr, "[MessageBoxA-stack] ASCII runs in [esp+0x10..esp+0x400]:\n");
        for (uint32_t a = esp + 0x10; a < esp + 0x400; ) {
            char buf[256]; int n = 0;
            uint32_t start = a;
            while (n < 255 && a < esp + 0x400) {
                char c = (char)win32_mem_read8(a++);
                if (c < 0x20 || c > 0x7e) { if (n) break; continue; }
                buf[n++] = c;
            }
            buf[n] = 0;
            if (n >= 6) {
                fprintf(stderr, "  [mb-str @0x%08x (len %d)] \"%s\"\n", start, n, buf);
            } else if (!n) a += 4; else a++;
        }
    }
    s.gpr.rax.dword = 1;   // IDOK
    win32_stdcall_ret(s, 4);
}
USTUB3(EnumThreadWindows)          // (dwThreadId, lpfn, lParam)
USTUB1(GetMenuItemCount)           // (hMenu)
USTUB2(GetMenuItemID)              // (hMenu, nPos)
USTUB3(DeleteMenu)                 // (hMenu, uPosition, uFlags)
USTUB5(GetUserObjectInformationW)  // (hObj, nIndex, pvInfo, nLength, pnLenNeeded)

// --- REGISTRATION ---
static const ImportEntry user32_impls[] = {
    {"GetSystemMetrics", impl_GetSystemMetrics},
    {"EnumDisplaySettingsA", impl_EnumDisplaySettingsA},
    {"ChangeDisplaySettingsA", impl_ChangeDisplaySettingsA},
    {"GetMonitorInfoA", impl_GetMonitorInfoA},
    {"MonitorFromWindow", impl_MonitorFromWindow},
    {"RegisterClassA", impl_RegisterClassA},
    {"RegisterClassExA", impl_RegisterClassExA},
    {"RegisterWindowMessageA", impl_RegisterWindowMessageA},
    {"CreateWindowExA", impl_CreateWindowExA},
    {"ShowWindow", impl_ShowWindow},
    {"DestroyWindow", impl_DestroyWindow},
    {"CloseWindow", impl_CloseWindow},
    {"MoveWindow", impl_MoveWindow},
    {"SetWindowPos", impl_SetWindowPos},
    {"SetWindowTextA", impl_SetWindowTextA},
    {"GetWindowTextA", impl_GetWindowTextA},
    {"GetClientRect", impl_GetClientRect},
    {"GetWindowRect", impl_GetWindowRect},
    {"AdjustWindowRect", impl_AdjustWindowRect},
    {"AdjustWindowRectEx", impl_AdjustWindowRectEx},
    {"ClientToScreen", impl_ClientToScreen},
    {"ScreenToClient", impl_ScreenToClient},
    {"GetMessageA", impl_GetMessageA},
    {"PeekMessageA", impl_PeekMessageA},
    {"TranslateMessage", impl_TranslateMessage},
    {"DispatchMessageA", impl_DispatchMessageA},
    {"PostQuitMessage", impl_PostQuitMessage},
    {"PostMessageA", impl_PostMessageA},
    {"SendMessageA", impl_SendMessageA},
    {"DefWindowProcA", impl_DefWindowProcA},
    {"CallWindowProcA", impl_CallWindowProcA},
    {"GetCursorPos", impl_GetCursorPos},
    {"SetCursorPos", impl_SetCursorPos},
    {"SetCursor", impl_SetCursor},
    {"ShowCursor", impl_ShowCursor},
    {"MapVirtualKeyA", impl_MapVirtualKeyA},
    {"GetDC", impl_GetDC},
    {"ReleaseDC", impl_ReleaseDC},
    {"LoadCursorA", impl_LoadCursorA},
    {"LoadImageA", impl_LoadImageA},
    {"LoadIconA", impl_LoadIconA},
    {"GetForegroundWindow", impl_GetForegroundWindow},
    {"GetActiveWindow", impl_GetActiveWindow},
    {"GetDesktopWindow", impl_GetDesktopWindow},
    {"GetProcessWindowStation", impl_GetProcessWindowStation},
    {"GetSystemMenu", impl_GetSystemMenu},
    {"SetFocus", impl_SetFocus},
    {"IsWindow", impl_IsWindow},
    {"GetWindowLongA", impl_GetWindowLongA},
    {"SetWindowLongA", impl_SetWindowLongA},
    {"GetClassLongA", impl_GetClassLongA},
    {"SetClassLongA", impl_SetClassLongA},
    {"OpenClipboard", impl_OpenClipboard},
    {"CloseClipboard", impl_CloseClipboard},
    {"GetClipboardData", impl_GetClipboardData},
    {"MessageBoxA", impl_MessageBoxA},
    {"EnumThreadWindows", impl_EnumThreadWindows},
    {"GetMenuItemCount", impl_GetMenuItemCount},
    {"GetMenuItemID", impl_GetMenuItemID},
    {"DeleteMenu", impl_DeleteMenu},
    {"GetUserObjectInformationW", impl_GetUserObjectInformationW},
    {nullptr, nullptr},
};

extern "C" const ImportEntry* get_user32_impls() {
    return user32_impls;
}
