// Window Highlighter: flashes a border/tint around the window that just got focus.
// Event driven (SetWinEventHook) - no polling; timers only run while the flash is visible.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <math.h>
#include <wchar.h>

enum { MODE_BORDER, MODE_TINT, MODE_BOTH };

typedef struct {
    int enabled, mode, opacity, tintOpacity, thickness, softness, holdMs, fadeMs;
    COLORREF color;
    WORD hotkey;            // HKM_* format: LOBYTE = vk, HIBYTE = HOTKEYF_* flags
    WCHAR ignore[256];      // ';' separated exe names
} Config;

#define APP_NAME   L"Window Highlighter"
#define MAIN_CLASS L"WindowHighlighterMain"
#define WM_TRAY    (WM_APP + 1)
#define WM_OPEN    (WM_APP + 2)

enum { T_DEBOUNCE = 1, T_HOLD, T_FADE };
enum { IDM_ENABLE = 1, IDM_SETTINGS, IDM_STARTUP, IDM_EXIT };
enum { IDC_ENABLE = 101, IDC_MODE, IDC_SWATCH, IDC_PICK, IDC_HOTKEY, IDC_HKCLEAR, IDC_IGNORE, IDC_STARTUP, IDC_PBOX,
       IDC_OPACITY = 110, IDC_TINT, IDC_THICK, IDC_SOFT, IDC_HOLD, IDC_FADE };   // slider value labels use id + 100

static HINSTANCE hInst;
static HWND hMain, hOverlay, hDlg;
static Config cfg;
static WCHAR iniPath[MAX_PATH];
static UINT wmTaskbar;
static NOTIFYICONDATAW nid;

// overlay state
static HDC g_mem;
static HBITMAP g_bmp;
static HGDIOBJ g_old;
static int g_hold, g_fade;
static DWORD g_fadeStart;

static void Defaults(Config *c) {
    c->enabled = 1; c->mode = MODE_BORDER; c->opacity = 90; c->tintOpacity = 20;
    c->thickness = 4; c->softness = 50; c->holdMs = 350; c->fadeMs = 250;
    c->color = RGB(51, 153, 255);
    c->hotkey = MAKEWORD('H', HOTKEYF_CONTROL | HOTKEYF_ALT);
    c->ignore[0] = 0;
}

static int Clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void Sanitize(Config *c) {
    c->enabled = c->enabled != 0;
    c->mode = Clampi(c->mode, 0, 2);
    c->opacity = Clampi(c->opacity, 1, 100);
    c->tintOpacity = Clampi(c->tintOpacity, 1, 100);
    c->thickness = Clampi(c->thickness, 1, 50);
    c->softness = Clampi(c->softness, 0, 100);
    c->holdMs = Clampi(c->holdMs, 0, 10000);
    c->fadeMs = Clampi(c->fadeMs, 0, 5000);
}

// ---------------------------------------------------------------- config
static void InitIniPath(void) {
    WCHAR dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { GetModuleFileNameW(NULL, dir, MAX_PATH); WCHAR *s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
    else { lstrcatW(dir, L"\\WindowHighlighter"); CreateDirectoryW(dir, NULL); }
    lstrcpyW(iniPath, dir); lstrcatW(iniPath, L"\\settings.ini");
}

static int GI(const WCHAR *k, int d) { return (int)GetPrivateProfileIntW(L"Highlight", k, d, iniPath); }
static void PI(const WCHAR *k, int v) {
    WCHAR b[16]; wsprintfW(b, L"%d", v); WritePrivateProfileStringW(L"Highlight", k, b, iniPath);
}

static void LoadConfig(void) {
    Config d; Defaults(&d);
    cfg.enabled = GI(L"Enabled", d.enabled);
    cfg.mode = GI(L"Mode", d.mode);
    cfg.opacity = GI(L"Opacity", d.opacity);
    cfg.tintOpacity = GI(L"TintOpacity", d.tintOpacity);
    cfg.thickness = GI(L"Thickness", d.thickness);
    cfg.softness = GI(L"Softness", d.softness);
    cfg.holdMs = GI(L"HoldMs", d.holdMs);
    cfg.fadeMs = GI(L"FadeMs", d.fadeMs);
    cfg.hotkey = (WORD)GI(L"Hotkey", d.hotkey);
    WCHAR b[32];
    GetPrivateProfileStringW(L"Highlight", L"Color", L"3399FF", b, 32, iniPath);
    DWORD v = wcstoul(b, NULL, 16);
    cfg.color = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
    GetPrivateProfileStringW(L"Highlight", L"Ignore", L"", cfg.ignore, 256, iniPath);
    Sanitize(&cfg);
}

static void SaveConfig(void) {
    PI(L"Enabled", cfg.enabled); PI(L"Mode", cfg.mode); PI(L"Opacity", cfg.opacity);
    PI(L"TintOpacity", cfg.tintOpacity); PI(L"Thickness", cfg.thickness); PI(L"Softness", cfg.softness);
    PI(L"HoldMs", cfg.holdMs); PI(L"FadeMs", cfg.fadeMs); PI(L"Hotkey", cfg.hotkey);
    WCHAR b[16];
    wsprintfW(b, L"%02X%02X%02X", GetRValue(cfg.color), GetGValue(cfg.color), GetBValue(cfg.color));
    WritePrivateProfileStringW(L"Highlight", L"Color", b, iniPath);
    WritePrivateProfileStringW(L"Highlight", L"Ignore", cfg.ignore, iniPath);
}

// ---------------------------------------------------------------- startup (HKCU Run)
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
static int IsStartup(void) {
    HKEY k; int r = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        r = RegQueryValueExW(k, L"WindowHighlighter", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return r;
}
static void SetStartup(int on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR p[MAX_PATH + 2];
        p[0] = L'"';
        DWORD n = GetModuleFileNameW(NULL, p + 1, MAX_PATH);
        p[n + 1] = L'"'; p[n + 2] = 0;
        RegSetValueExW(k, L"WindowHighlighter", 0, REG_SZ, (BYTE *)p, (n + 3) * sizeof(WCHAR));
    } else RegDeleteValueW(k, L"WindowHighlighter");
    RegCloseKey(k);
}

// ---------------------------------------------------------------- rendering
static float Sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Fills premultiplied ARGB pixels: rounded-rect border band and/or interior tint.
// `outside`: border band lies outside the window, so it fades toward its outer edge (a glow);
// otherwise it fades toward the window interior.
// `thick` is the band thickness in px (may exceed c->thickness to overlap the window's own edge).
static void Render(DWORD *px, int w, int h, const Config *c, float r, int outside, int thick) {
    float t = (float)thick;
    float Ab = (c->mode == MODE_TINT ? c->tintOpacity : c->opacity) / 100.f;
    float At = c->mode == MODE_BORDER ? 0.f : c->tintOpacity / 100.f;
    int cr = GetRValue(c->color), cg = GetGValue(c->color), cb = GetBValue(c->color);
    float hw = w * .5f, hh = h * .5f;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float ex = hw - r, ey = hh - r;

    DWORD tintPx = 0;
    if (At > 0) {
        int A = (int)(At * 255 + .5f);
        tintPx = (A << 24) | ((cr * A / 255) << 16) | ((cg * A / 255) << 8) | (cb * A / 255);
    }
    float soft = c->mode == MODE_TINT ? 0.f : c->softness / 100.f;
    float inner = ex - t - 1;   // |px| below this (on inner rows) is pure interior

    for (int y = 0; y < h; y++) {
        float qy = fabsf(y + .5f - hh) - ey;
        DWORD *row = px + (size_t)y * w;
        int x0 = w, x1 = -1;
        if (qy < -(t + 1) && inner > 0) {
            x0 = (int)(hw - inner) + 1; x1 = (int)(hw + inner) - 1;
            if (x0 < 0) x0 = 0;
            if (x1 >= w) x1 = w - 1;
            if (tintPx) for (int x = x0; x <= x1; x++) row[x] = tintPx;
        }
        for (int x = 0; x < w; x++) {
            if (x == x0) { x = x1; continue; }
            float qx = fabsf(x + .5f - hw) - ex;
            float mx = fmaxf(qx, 0.f), my = fmaxf(qy, 0.f);
            float d = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0.f) - r;
            float co = Sat(.5f - d), ci = Sat(.5f - (d + t));
            float f = 1.f;
            if (soft > 0) {
                float k = Sat(-d / t);                       // 0 at outer edge .. 1 at inner edge
                f = Sat((outside ? k : 1 - k) / soft);
            }
            float a = co * (Ab * f * (1 - ci) + At * ci);
            int A = (int)(a * 255 + .5f);
            if (A) row[x] = (A << 24) | ((cr * A / 255) << 16) | ((cg * A / 255) << 8) | (cb * A / 255);
        }
    }
}

static void HideOverlay(void) {
    KillTimer(hMain, T_HOLD); KillTimer(hMain, T_FADE);
    ShowWindow(hOverlay, SW_HIDE);
    if (g_mem) { SelectObject(g_mem, g_old); DeleteObject(g_bmp); DeleteDC(g_mem); g_mem = NULL; g_bmp = NULL; }
}

// Windows 11 rounds top-level window corners by 8 px (scaled with DPI); Windows 10 and
// maximized/full-screen windows are square. Honors a per-window "no rounding" / "small rounding" preference.
static int g_win11 = -1;
static int WindowRadius(HWND w) {
    if (g_win11 < 0) {
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW vi = { sizeof vi };
        g_win11 = fn && fn(&vi) == 0 && vi.dwBuildNumber >= 22000;
    }
    if (!g_win11) return 0;
    int pref = 0;   // DWMWA_WINDOW_CORNER_PREFERENCE: 0 default, 1 none, 2 round, 3 small
    DwmGetWindowAttribute(w, 33, &pref, sizeof pref);
    int base = pref == 1 ? 0 : pref == 3 ? 4 : 8;
    return MulDiv(base, GetDpiForWindow(w), 96);
}

static void ShowHighlight(HWND target, const Config *c) {
    RECT r;
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)))
        GetWindowRect(target, &r);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    if (ww < 8 || wh < 8) return;
    HideOverlay();

    // Windowed: the border sits outside the window edge (overlay grows by `pad`).
    // Maximized / full screen: no room outside, so the border stays inside.
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi);
    int full = IsZoomed(target) || (r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                                    r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom);
    int pad = (full || c->mode == MODE_TINT) ? 0 : c->thickness;
    // DWM draws a ~1px border line at the window edge; overlap it so no sliver shows between border and window.
    int ov = pad ? MulDiv(2, GetDpiForWindow(target), 96) : 0;
    int w = ww + 2 * pad, h = wh + 2 * pad;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC scr = GetDC(NULL);
    g_mem = CreateCompatibleDC(scr);
    g_bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, scr);
    if (!g_bmp || !bits) { if (g_bmp) DeleteObject(g_bmp); DeleteDC(g_mem); g_mem = NULL; g_bmp = NULL; return; }
    g_old = SelectObject(g_mem, g_bmp);

    float radius = full ? 0.f : (float)(WindowRadius(target) + pad);
    Render((DWORD *)bits, w, h, c, radius, pad > 0, c->thickness + ov);

    g_hold = c->holdMs; g_fade = c->fadeMs;
    SetWindowPos(hOverlay, HWND_TOPMOST, r.left - pad, r.top - pad, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    POINT dst = { r.left - pad, r.top - pad }, src = {0, 0};
    SIZE sz = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hOverlay, NULL, &dst, &sz, g_mem, &src, 0, &bf, ULW_ALPHA);
    SetTimer(hMain, T_HOLD, g_hold < USER_TIMER_MINIMUM ? USER_TIMER_MINIMUM : g_hold, NULL);
}

static void OnFade(void) {
    DWORD el = GetTickCount() - g_fadeStart;
    if (el >= (DWORD)g_fade) { HideOverlay(); return; }
    POINT src = {0, 0};
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(255 - 255 * el / g_fade), AC_SRC_ALPHA };
    UpdateLayeredWindow(hOverlay, NULL, NULL, NULL, g_mem, &src, 0, &bf, ULW_ALPHA);
}

// ---------------------------------------------------------------- focus handling
static const WCHAR *const kIgnoredClasses[] = {
    L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"TaskSwitcherWnd", L"XamlExplorerHostIslandWindow",
    L"MultitaskingViewFrame", L"ForegroundStaging", L"Windows.UI.Core.CoreWindow",
    L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland", L"#32768",
    L"Progman", L"WorkerW", NULL
};

static int IgnoredProcess(HWND h) {
    if (!cfg.ignore[0]) return 0;
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    WCHAR path[MAX_PATH]; DWORD n = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(p, 0, path, &n);
    CloseHandle(p);
    if (!ok) return 0;
    WCHAR *base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
    size_t bl = wcslen(base);
    for (const WCHAR *s = cfg.ignore; *s;) {
        while (*s == L';' || *s == L' ') s++;
        const WCHAR *st = s;
        while (*s && *s != L';') s++;
        const WCHAR *e = s;
        while (e > st && e[-1] == L' ') e--;
        if ((size_t)(e - st) == bl && bl && _wcsnicmp(base, st, bl) == 0) return 1;
    }
    return 0;
}

static void OnFocus(void) {
    if (!cfg.enabled) return;
    HWND h = GetForegroundWindow();
    if (!h || h == hMain || h == hOverlay || !IsWindowVisible(h) || IsIconic(h)) return;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return;
    WCHAR cls[64];
    if (GetClassNameW(h, cls, 64))
        for (int i = 0; kIgnoredClasses[i]; i++) if (!lstrcmpW(cls, kIgnoredClasses[i])) return;
    if (IgnoredProcess(h)) return;
    ShowHighlight(h, &cfg);
}

static void CALLBACK WinEvt(HWINEVENTHOOK hk, DWORD ev, HWND hw, LONG o, LONG c, DWORD t, DWORD tm) {
    SetTimer(hMain, T_DEBOUNCE, 40, NULL);   // re-arming = debounce; lets restore animations settle
}

// ---------------------------------------------------------------- tray / hotkey
static HICON MakeIcon(void) {
    int s = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = s; bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC scr = GetDC(NULL);
    HBITMAP color = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, scr);
    Config t; Defaults(&t);
    t.mode = MODE_BORDER; t.opacity = 100; t.thickness = s / 8 > 2 ? s / 8 : 2;
    t.softness = 0;
    Render((DWORD *)bits, s, s, &t, s / 5.f, 0, t.thickness);
    DWORD *p = bits;                       // icons want straight (non-premultiplied) alpha
    for (int i = 0; i < s * s; i++) {
        DWORD a = p[i] >> 24;
        if (a && a < 255) {
            DWORD r = ((p[i] >> 16) & 255) * 255 / a, g = ((p[i] >> 8) & 255) * 255 / a, b = (p[i] & 255) * 255 / a;
            p[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    HBITMAP mask = CreateBitmap(s, s, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}

static void UpdateTip(void) {
    lstrcpyW(nid.szTip, cfg.enabled ? APP_NAME : APP_NAME L" (paused)");
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void AddTray(void) {
    nid.cbSize = sizeof nid; nid.hWnd = hMain; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    if (!nid.hIcon) nid.hIcon = MakeIcon();
    lstrcpyW(nid.szTip, cfg.enabled ? APP_NAME : APP_NAME L" (paused)");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RegisterHotkey(void) {
    UnregisterHotKey(hMain, 1);
    UINT vk = LOBYTE(cfg.hotkey), m = HIBYTE(cfg.hotkey);
    if (!vk) return;
    UINT mods = (m & HOTKEYF_SHIFT ? MOD_SHIFT : 0) | (m & HOTKEYF_CONTROL ? MOD_CONTROL : 0) |
                (m & HOTKEYF_ALT ? MOD_ALT : 0) | MOD_NOREPEAT;
    RegisterHotKey(hMain, 1, mods, vk);
}

// ---------------------------------------------------------------- dark mode
// Follows the system "app mode" setting. Uses undocumented uxtheme ordinals (same ones Explorer uses)
// for dark popup menus; everything else is documented API plus WM_CTLCOLOR* painting.
static int g_dark;
static HBRUSH g_brBg, g_brEdit, g_brSide;
static int (WINAPI *pSetPreferredAppMode)(int);
static BOOL (WINAPI *pAllowDarkModeForWindow)(HWND, BOOL);
static void (WINAPI *pFlushMenuThemes)(void);

#define C_BG()     (g_dark ? RGB(32, 32, 32) : RGB(249, 249, 249))
#define C_SIDE()   (g_dark ? RGB(40, 40, 40) : RGB(238, 238, 238))
#define C_EDIT()   (g_dark ? RGB(45, 45, 45) : RGB(255, 255, 255))
#define C_TEXT()   (g_dark ? RGB(240, 240, 240) : RGB(20, 20, 20))
#define C_MUTED()  (g_dark ? RGB(160, 160, 160) : RGB(100, 100, 100))
#define C_SEL()    (g_dark ? RGB(58, 58, 58) : RGB(222, 222, 222))
#define C_ACCENT() (g_dark ? RGB(96, 205, 255) : RGB(0, 103, 192))
#define C_PANEL()  (g_dark ? RGB(22, 22, 22) : RGB(228, 228, 228))

static void RefreshDark(void) {
    DWORD v = 1, n = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n);
    g_dark = v == 0;
    if (g_brBg) { DeleteObject(g_brBg); DeleteObject(g_brSide); DeleteObject(g_brEdit); }
    g_brBg = CreateSolidBrush(C_BG());
    g_brSide = CreateSolidBrush(C_SIDE());
    g_brEdit = CreateSolidBrush(C_EDIT());
    if (pFlushMenuThemes) pFlushMenuThemes();
}

static void InitDark(void) {
    HMODULE u = LoadLibraryW(L"uxtheme.dll");
    if (u) {
        pSetPreferredAppMode = (void *)GetProcAddress(u, MAKEINTRESOURCEA(135));
        pAllowDarkModeForWindow = (void *)GetProcAddress(u, MAKEINTRESOURCEA(133));
        pFlushMenuThemes = (void *)GetProcAddress(u, MAKEINTRESOURCEA(136));
    }
    if (pSetPreferredAppMode) pSetPreferredAppMode(1);   // AllowDark: menus follow the system
    RefreshDark();
}

static BOOL CALLBACK ThemeChild(HWND c, LPARAM l) {
    WCHAR cls[32];
    GetClassNameW(c, cls, 32);
    int btn = !lstrcmpiW(cls, L"Button"), trk = !lstrcmpiW(cls, TRACKBAR_CLASSW);
    int edt = !lstrcmpiW(cls, L"Edit") || !lstrcmpiW(cls, L"ComboBox") || !lstrcmpiW(cls, HOTKEY_CLASSW);
    if (btn || trk || edt)
        SetWindowTheme(c, g_dark ? ((btn || trk) ? L"DarkMode_Explorer" : L"DarkMode_CFD") : NULL, NULL);
    return TRUE;
}

static void ApplyDlgTheme(HWND h) {
    BOOL d = g_dark;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof d);
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(h, d);
    EnumChildWindows(h, ThemeChild, 0);
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static int IsColorSetChange(LPARAM l) { return l && !lstrcmpW((LPCWSTR)l, L"ImmersiveColorSet"); }

// ---------------------------------------------------------------- settings dialog
// Sidebar (custom painted) + pages of child controls + a live preview panel above the page.
enum { PG_APPEAR, PG_TIMING, PG_BEHAVIOR, PG_IGNORE, PG_COUNT };
static const WCHAR *const kPageNames[] = { L"Appearance", L"Timing", L"Behavior", L"Ignore list" };
#define SIDE_W   140
#define CLIENT_W 580
#define CLIENT_H 396
#define CX       164     // content left edge
#define CTL_X    276     // control column
#define SAVE_TIMER 1
#define ANIM_TIMER 2
#define ANIM_GAP_MS 700    // pause between preview loops on the Timing page

static int g_dpi = 96;
static HFONT g_font;
static COLORREF dlgColor;
static HWND hSwatch, hPrev;
static COLORREF custColors[16];
static int g_page, g_loading, g_dirty;
static int g_prevAlpha = 255;    // preview highlight opacity (animated on the Timing page)
static DWORD g_animStart;
static HWND ctl[64];
static int ctlPage[64], nctl, creatingPage = -1;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND Mk(LPCWSTR cls, LPCWSTR text, DWORD style, DWORD ex, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             hDlg, (HMENU)(INT_PTR)id, hInst, NULL);
    if (nctl < 64) { ctl[nctl] = c; ctlPage[nctl++] = creatingPage; }
    return c;
}
static void Label(LPCWSTR t, int y) { Mk(L"STATIC", t, SS_LEFT | SS_CENTERIMAGE, 0, CX, y, 106, 28, -1); }

typedef struct { int id, lo, hi, step; const WCHAR *unit; } SliderDef;
static const SliderDef kSliders[] = {
    { IDC_OPACITY, 1, 100, 1, L"%" }, { IDC_TINT, 1, 100, 1, L"%" }, { IDC_THICK, 1, 30, 1, L" px" },
    { IDC_SOFT, 0, 100, 1, L"%" },   { IDC_HOLD, 0, 60, 50, L" ms" }, { IDC_FADE, 0, 40, 50, L" ms" },
};
static const SliderDef *FindSlider(int id) {
    for (int i = 0; i < (int)(sizeof kSliders / sizeof *kSliders); i++) if (kSliders[i].id == id) return &kSliders[i];
    return NULL;
}
static int SliderVal(int id) {
    return (int)SendDlgItemMessageW(hDlg, id, TBM_GETPOS, 0, 0) * FindSlider(id)->step;
}
static void UpdateValueLabel(int id) {
    if (!FindSlider(id)) return;
    WCHAR b[32];
    wsprintfW(b, L"%d", SliderVal(id));
    lstrcatW(b, FindSlider(id)->unit);
    SetDlgItemTextW(hDlg, id + 100, b);
}
static void SliderRow(LPCWSTR text, int id, int y) {
    const SliderDef *d = FindSlider(id);
    Label(text, y);
    HWND t = Mk(L"WHSlider", L"", WS_TABSTOP, 0, CTL_X - 6, y, 210, 28, id);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(d->lo, d->hi));
    Mk(L"STATIC", L"", SS_RIGHT | SS_CENTERIMAGE, 0, CTL_X + 210, y, 68, 28, id + 100);
}

static BOOL CALLBACK SetFont(HWND h, LPARAM f) { SendMessageW(h, WM_SETFONT, f, TRUE); return TRUE; }

static void ReadDialog(Config *c) {
    c->enabled = IsDlgButtonChecked(hDlg, IDC_ENABLE) == BST_CHECKED;
    c->mode = (int)SendDlgItemMessageW(hDlg, IDC_MODE, CB_GETCURSEL, 0, 0);
    c->color = dlgColor;
    c->opacity = SliderVal(IDC_OPACITY); c->tintOpacity = SliderVal(IDC_TINT);
    c->thickness = SliderVal(IDC_THICK); c->softness = SliderVal(IDC_SOFT);
    c->holdMs = SliderVal(IDC_HOLD); c->fadeMs = SliderVal(IDC_FADE);
    c->hotkey = LOWORD(SendDlgItemMessageW(hDlg, IDC_HOTKEY, HKM_GETHOTKEY, 0, 0));
    GetDlgItemTextW(hDlg, IDC_IGNORE, c->ignore, 256);
    Sanitize(c);
}

static void SetSliderVal(int id, int v) {
    const SliderDef *d = FindSlider(id);
    SendDlgItemMessageW(hDlg, id, TBM_SETPOS, TRUE, Clampi((v + d->step / 2) / d->step, d->lo, d->hi));
    UpdateValueLabel(id);
}

static void PutDialog(const Config *c) {
    g_loading = 1;
    CheckDlgButton(hDlg, IDC_ENABLE, c->enabled ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(hDlg, IDC_MODE, CB_SETCURSEL, c->mode, 0);
    dlgColor = c->color;
    SetSliderVal(IDC_OPACITY, c->opacity); SetSliderVal(IDC_TINT, c->tintOpacity);
    SetSliderVal(IDC_THICK, c->thickness); SetSliderVal(IDC_SOFT, c->softness);
    SetSliderVal(IDC_HOLD, c->holdMs); SetSliderVal(IDC_FADE, c->fadeMs);
    SendDlgItemMessageW(hDlg, IDC_HOTKEY, HKM_SETHOTKEY, c->hotkey, 0);
    SetDlgItemTextW(hDlg, IDC_IGNORE, c->ignore);
    CheckDlgButton(hDlg, IDC_STARTUP, IsStartup() ? BST_CHECKED : BST_UNCHECKED);
    InvalidateRect(hSwatch, NULL, TRUE);
    g_loading = 0;
}

// Persisting (ini write, registry, hotkey) is debounced so dragging a slider stays smooth.
static void FlushSave(void) {
    if (!g_dirty) return;
    g_dirty = 0;
    KillTimer(hDlg, SAVE_TIMER);
    SaveConfig();
    SetStartup(IsDlgButtonChecked(hDlg, IDC_STARTUP) == BST_CHECKED);
    RegisterHotkey();
    UpdateTip();
}

// Every control change takes effect immediately; the preview redraws at once.
static void LiveApply(void) {
    if (g_loading) return;
    ReadDialog(&cfg);
    if (!cfg.enabled) HideOverlay();
    if (hPrev) InvalidateRect(hPrev, NULL, FALSE);
    g_dirty = 1;
    SetTimer(hDlg, SAVE_TIMER, 250, NULL);
}

// Timing page: loop show -> fade -> pause using the configured durations.
static void AnimTick(void) {
    DWORD cycle = (DWORD)(cfg.holdMs + cfg.fadeMs + ANIM_GAP_MS);
    DWORD t = (GetTickCount() - g_animStart) % cycle;
    int a = t < (DWORD)cfg.holdMs ? 255
          : t < (DWORD)(cfg.holdMs + cfg.fadeMs) ? (int)(255 - 255 * (t - cfg.holdMs) / cfg.fadeMs) : 0;
    if (a != g_prevAlpha) { g_prevAlpha = a; if (hPrev) InvalidateRect(hPrev, NULL, FALSE); }
}

static void SetPage(int p) {
    g_page = p;
    if (p == PG_TIMING) { g_animStart = GetTickCount(); SetTimer(hDlg, ANIM_TIMER, 16, NULL); }
    else { KillTimer(hDlg, ANIM_TIMER); if (g_prevAlpha != 255) { g_prevAlpha = 255; if (hPrev) InvalidateRect(hPrev, NULL, FALSE); } }
    for (int i = 0; i < nctl; i++)
        if (ctlPage[i] >= 0) ShowWindow(ctl[i], ctlPage[i] == p ? SW_SHOWNA : SW_HIDE);
    RECT rc = { 0, 0, S(SIDE_W), S(CLIENT_H) };
    InvalidateRect(hDlg, &rc, FALSE);
}

static void PaintSidebar(HDC dc) {
    RECT side = { 0, 0, S(SIDE_W), S(CLIENT_H) };
    FillRect(dc, &side, g_brSide);
    HGDIOBJ of = SelectObject(dc, g_font), op = SelectObject(dc, GetStockObject(NULL_PEN));
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < PG_COUNT; i++) {
        RECT it = { S(8), S(16 + i * 40), S(SIDE_W - 8), S(16 + i * 40 + 34) };
        if (i == g_page) {
            HBRUSH b = CreateSolidBrush(C_SEL()), ab = CreateSolidBrush(C_ACCENT());
            HGDIOBJ ob = SelectObject(dc, b);
            RoundRect(dc, it.left, it.top, it.right + 1, it.bottom + 1, S(8), S(8));
            SelectObject(dc, ob); DeleteObject(b);
            RECT bar = { it.left, it.top + S(9), it.left + S(3), it.bottom - S(9) };
            FillRect(dc, &bar, ab); DeleteObject(ab);
        }
        SetTextColor(dc, i == g_page ? C_TEXT() : C_MUTED());
        RECT tr = it; tr.left += S(16);
        DrawTextW(dc, kPageNames[i], -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
    SelectObject(dc, op); SelectObject(dc, of);
}

static HBITMAP NewDIB(HDC like, int w, int h, void **bits) {
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    return CreateDIBSection(like, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

// Preview: a mock window with the current highlight drawn exactly as the overlay would draw it.
static void PaintPreview(HWND h, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    int pw = rc.right, ph = rc.bottom;
    void *bits = NULL;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = NewDIB(dc, pw, ph, &bits);
    HGDIOBJ old = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(C_PANEL());
    FillRect(mem, &rc, bg); DeleteObject(bg);

    int ww = S(180), wh = S(60), x = (pw - ww) / 2, y = (ph - wh) / 2;
    int rad = WindowRadius(h);
    HBRUSH fb = CreateSolidBrush(g_dark ? RGB(43, 43, 43) : RGB(255, 255, 255));
    HPEN fp = CreatePen(PS_SOLID, 1, g_dark ? RGB(72, 72, 72) : RGB(200, 200, 200));
    HGDIOBJ ob = SelectObject(mem, fb), opn = SelectObject(mem, fp);
    RoundRect(mem, x, y, x + ww, y + wh, 2 * rad, 2 * rad);
    SelectObject(mem, ob); SelectObject(mem, opn); DeleteObject(fb); DeleteObject(fp);
    HBRUSH lb = CreateSolidBrush(g_dark ? RGB(74, 74, 74) : RGB(214, 214, 214));
    int lens[3] = { 90, 140, 110 };
    for (int i = 0; i < 3; i++) {
        RECT l = { x + S(14), y + S(14 + i * 14), x + S(14 + lens[i]), y + S(20 + i * 14) };
        FillRect(mem, &l, lb);
    }
    DeleteObject(lb);

    int pad = cfg.mode == MODE_TINT ? 0 : cfg.thickness;
    int ov = pad ? MulDiv(2, g_dpi, 96) : 0;
    int hw = ww + 2 * pad, hh = wh + 2 * pad;
    void *hbits = NULL;
    HDC hm = CreateCompatibleDC(dc);
    HBITMAP hb = NewDIB(dc, hw, hh, &hbits);
    if (hb && hbits) {
        HGDIOBJ ho = SelectObject(hm, hb);
        Render((DWORD *)hbits, hw, hh, &cfg, (float)(rad + pad), pad > 0, cfg.thickness + ov);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)g_prevAlpha, AC_SRC_ALPHA };
        AlphaBlend(mem, x - pad, y - pad, hw, hh, hm, 0, 0, hw, hh, bf);
        SelectObject(hm, ho);
    }
    if (hb) DeleteObject(hb);
    DeleteDC(hm);

    HGDIOBJ of = SelectObject(mem, g_font);
    SetBkMode(mem, TRANSPARENT); SetTextColor(mem, C_MUTED());
    RECT hr = { 0, ph - S(22), pw, ph - S(4) };
    DrawTextW(mem, L"Click to flash this window", -1, &hr, DT_CENTER | DT_SINGLELINE);
    SelectObject(mem, of);

    BitBlt(dc, 0, 0, pw, ph, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
}

static LRESULT CALLBACK PrevProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintPreview(h, dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND)); return TRUE;
    case WM_LBUTTONDOWN: if (hDlg) ShowHighlight(hDlg, &cfg); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- custom slider (replaces the stock trackbar so it looks right in both themes)
// Understands TBM_SETRANGE / TBM_SETPOS / TBM_GETPOS and notifies the parent with WM_HSCROLL.
typedef struct { int lo, hi, pos, hover, drag; } SliderState;

static float Cov(float d) { return Sat(.5f - d); }
static DWORD Mix(DWORD bg, COLORREF c, float a) {
    int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    int r = (int)(br + (GetRValue(c) - br) * a + .5f), g = (int)(bgc + (GetGValue(c) - bgc) * a + .5f),
        b = (int)(bb + (GetBValue(c) - bb) * a + .5f);
    return (r << 16) | (g << 8) | b;
}

static void SliderGeom(HWND h, float *x0, float *x1) {
    RECT rc; GetClientRect(h, &rc);
    float m = 11 * g_dpi / 96.f;
    *x0 = m; *x1 = rc.right - m;
}

static float SliderThumbX(HWND h, const SliderState *s) {
    float x0, x1; SliderGeom(h, &x0, &x1);
    return x0 + (x1 - x0) * (s->hi > s->lo ? (float)(s->pos - s->lo) / (s->hi - s->lo) : 0.f);
}

static void PaintSlider(HWND h, HDC dc, const SliderState *st) {
    RECT rc; GetClientRect(h, &rc);
    int w = rc.right, hh = rc.bottom;
    void *bits = NULL;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = NewDIB(dc, w, hh, &bits);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(mem); return; }
    HGDIOBJ old = SelectObject(mem, bmp);

    float k = g_dpi / 96.f, R = 8 * k, th = 4 * k, cy = hh * .5f;
    float x0, x1; SliderGeom(h, &x0, &x1);
    float tx = SliderThumbX(h, st);
    COLORREF bgc = C_BG(), accent = C_ACCENT();
    COLORREF track = g_dark ? RGB(118, 118, 118) : RGB(160, 160, 160);
    // Single-circle thumb: hover brightens it, pressed dims it.
    COLORREF thumb = st->drag ? RGB(GetRValue(accent) * 3 / 4, GetGValue(accent) * 3 / 4, GetBValue(accent) * 3 / 4)
                   : st->hover ? RGB(GetRValue(accent) + (255 - GetRValue(accent)) / 3,
                                     GetGValue(accent) + (255 - GetGValue(accent)) / 3,
                                     GetBValue(accent) + (255 - GetBValue(accent)) / 3)
                   : accent;
    DWORD bg = (GetRValue(bgc) << 16) | (GetGValue(bgc) << 8) | GetBValue(bgc);
    DWORD *px = bits;

    for (int y = 0; y < hh; y++) {
        float py = y + .5f;
        for (int x = 0; x < w; x++) {
            float pxx = x + .5f;
            float dxs = pxx < x0 ? x0 - pxx : pxx > x1 ? pxx - x1 : 0.f;
            float d = sqrtf(dxs * dxs + (py - cy) * (py - cy)) - th * .5f;
            DWORD c = bg;
            c = Mix(c, track, Cov(d));
            if (pxx < tx) c = Mix(c, accent, Cov(d));
            float dist = sqrtf((pxx - tx) * (pxx - tx) + (py - cy) * (py - cy));
            c = Mix(c, thumb, Cov(dist - R));
            px[y * w + x] = c;
        }
    }
    BitBlt(dc, 0, 0, w, hh, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
}

static void SliderSet(HWND h, SliderState *s, int pos, int notify) {
    pos = Clampi(pos, s->lo, s->hi);
    if (pos == s->pos) return;
    s->pos = pos;
    InvalidateRect(h, NULL, FALSE);
    if (notify) SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(SB_THUMBTRACK, pos), (LPARAM)h);
}

static void SliderFromMouse(HWND h, SliderState *s, int mx) {
    float x0, x1; SliderGeom(h, &x0, &x1);
    float f = Sat((mx - x0) / (x1 - x0));
    SliderSet(h, s, s->lo + (int)(f * (s->hi - s->lo) + .5f), 1);
}

static LRESULT CALLBACK SliderProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SliderState *s = (SliderState *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)LocalAlloc(LPTR, sizeof(SliderState)));
        break;
    case WM_NCDESTROY:
        LocalFree(s);
        break;
    case TBM_SETRANGE: s->lo = LOWORD(l); s->hi = HIWORD(l); s->pos = Clampi(s->pos, s->lo, s->hi); return 0;
    case TBM_SETPOS: s->pos = Clampi((int)l, s->lo, s->hi); InvalidateRect(h, NULL, FALSE); return 0;
    case TBM_GETPOS: return s->pos;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintSlider(h, dc, s); EndPaint(h, &ps); return 0; }
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h); SetCapture(h); s->drag = 1;
        SliderFromMouse(h, s, (short)LOWORD(l));
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (!s->hover) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            s->hover = 1; InvalidateRect(h, NULL, FALSE);
        }
        if (s->drag) SliderFromMouse(h, s, (short)LOWORD(l));
        return 0;
    case WM_MOUSELEAVE: s->hover = 0; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (s->drag) { s->drag = 0; if (GetCapture() == h) ReleaseCapture(); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN: {
        int span = s->hi - s->lo, step = span >= 50 ? span / 20 : 1;
        switch (w) {
        case VK_LEFT: case VK_DOWN: SliderSet(h, s, s->pos - 1, 1); return 0;
        case VK_RIGHT: case VK_UP: SliderSet(h, s, s->pos + 1, 1); return 0;
        case VK_PRIOR: SliderSet(h, s, s->pos + step, 1); return 0;
        case VK_NEXT: SliderSet(h, s, s->pos - step, 1); return 0;
        case VK_HOME: SliderSet(h, s, s->lo, 1); return 0;
        case VK_END: SliderSet(h, s, s->hi, 1); return 0;
        }
        break;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        hDlg = h; nctl = 0; creatingPage = -1; g_page = 0;
        hPrev = Mk(L"WHPreview", L"", 0, 0, CX, 16, CLIENT_W - CX - 16, 124, IDC_PBOX);
        int y0 = 160, y;

        creatingPage = PG_APPEAR; y = y0;
        Label(L"Style", y);
        HWND cb = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, CTL_X, y + 2, 180, 120, IDC_MODE);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Border");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Tint");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Border + tint"); y += 36;
        Label(L"Color", y);
        hSwatch = Mk(L"STATIC", L"", SS_NOTIFY | WS_BORDER, 0, CTL_X, y + 2, 44, 24, IDC_SWATCH);
        Mk(L"BUTTON", L"Choose...", BS_PUSHBUTTON | WS_TABSTOP, 0, CTL_X + 54, y + 1, 92, 26, IDC_PICK); y += 36;
        SliderRow(L"Border opacity", IDC_OPACITY, y); y += 36;
        SliderRow(L"Tint opacity", IDC_TINT, y); y += 36;
        SliderRow(L"Thickness", IDC_THICK, y); y += 36;
        SliderRow(L"Edge fade", IDC_SOFT, y);

        creatingPage = PG_TIMING; y = y0;
        SliderRow(L"Show for", IDC_HOLD, y); y += 36;
        SliderRow(L"Fade out", IDC_FADE, y);

        creatingPage = PG_BEHAVIOR; y = y0;
        Mk(L"BUTTON", L"Highlight when window focus changes", BS_AUTOCHECKBOX | WS_TABSTOP, 0, CX, y, 380, 28, IDC_ENABLE); y += 36;
        Mk(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 0, CX, y, 380, 28, IDC_STARTUP); y += 36;
        Label(L"Toggle hotkey", y);
        Mk(HOTKEY_CLASSW, L"", WS_TABSTOP, WS_EX_CLIENTEDGE, CTL_X, y + 1, 150, 26, IDC_HOTKEY);
        Mk(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, 0, CTL_X + 160, y + 1, 56, 26, IDC_HKCLEAR);

        creatingPage = PG_IGNORE; y = y0;
        Mk(L"STATIC", L"Windows from these programs are never highlighted. Enter exe names separated by semicolons, "
                      L"for example notepad.exe; chrome.exe", SS_LEFT, 0, CX, y, 400, 40, -1); y += 48;
        Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, CX, y, 400, 26, IDC_IGNORE);
        creatingPage = -1;

        g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
        EnumChildWindows(h, SetFont, (LPARAM)g_font);
        PutDialog(&cfg);
        SetPage(PG_APPEAR);
        ApplyDlgTheme(h);

        RECT rc = { 0, 0, S(CLIENT_W), S(CLIENT_H) };
        AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU, FALSE, 0, g_dpi);
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        SetWindowPos(h, NULL, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                     ww, wh, SWP_NOZORDER);
        return 0;
    }
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintSidebar(dc); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (x < S(SIDE_W)) {
            int i = (y - S(16)) / S(40);
            if (y >= S(16) && i >= 0 && i < PG_COUNT && i != g_page) SetPage(i);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == hSwatch) {
            SetDCBrushColor((HDC)w, dlgColor);
            return (LRESULT)GetStockObject(DC_BRUSH);
        }
        SetTextColor((HDC)w, C_TEXT()); SetBkColor((HDC)w, C_BG());
        return (LRESULT)g_brBg;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        if (g_dark) { SetTextColor((HDC)w, C_TEXT()); SetBkColor((HDC)w, C_EDIT()); return (LRESULT)g_brEdit; }
        break;
    case WM_ERASEBKGND: { RECT rc; GetClientRect(h, &rc); FillRect((HDC)w, &rc, g_brBg); return 1; }
    case WM_SETTINGCHANGE:
        if (IsColorSetChange(l)) { RefreshDark(); ApplyDlgTheme(h); }
        break;
    case WM_TIMER:
        if (w == SAVE_TIMER) FlushSave();
        else if (w == ANIM_TIMER) AnimTick();
        return 0;
    case WM_HSCROLL:
        UpdateValueLabel(GetDlgCtrlID((HWND)l));
        LiveApply();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_SWATCH: case IDC_PICK: {
            CHOOSECOLORW cc = { sizeof cc, h, NULL, dlgColor, custColors, CC_RGBINIT | CC_FULLOPEN };
            if (ChooseColorW(&cc)) { dlgColor = cc.rgbResult; InvalidateRect(hSwatch, NULL, TRUE); LiveApply(); }
            return 0;
        }
        case IDC_HKCLEAR: SendDlgItemMessageW(h, IDC_HOTKEY, HKM_SETHOTKEY, 0, 0); LiveApply(); return 0;
        case IDC_ENABLE: case IDC_STARTUP:
            if (HIWORD(w) == BN_CLICKED) LiveApply();
            return 0;
        case IDC_MODE:
            if (HIWORD(w) == CBN_SELCHANGE) LiveApply();
            return 0;
        case IDC_HOTKEY: case IDC_IGNORE:
            if (HIWORD(w) == EN_CHANGE) LiveApply();
            return 0;
        case IDCANCEL: DestroyWindow(h); return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        KillTimer(h, ANIM_TIMER); g_prevAlpha = 255;
        FlushSave();
        DeleteObject(g_font); g_font = NULL; hDlg = NULL; hPrev = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void OpenSettings(void) {
    if (hDlg) { SetForegroundWindow(hDlg); return; }
    g_dpi = GetDpiForSystem();
    CreateWindowExW(0, L"WHSettings", APP_NAME L" Settings", WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, hInst, NULL);
    if (hDlg) SetForegroundWindow(hDlg);
}

// ---------------------------------------------------------------- main window
static void TrayMenu(void) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (cfg.enabled ? MF_CHECKED : 0), IDM_ENABLE, L"Enabled");
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING | (IsStartup() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(hMain);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, hMain, NULL);
    PostMessageW(hMain, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == wmTaskbar) { AddTray(); return 0; }
    switch (m) {
    case WM_TIMER:
        if (w == T_DEBOUNCE) { KillTimer(h, T_DEBOUNCE); OnFocus(); }
        else if (w == T_HOLD) {
            KillTimer(h, T_HOLD);
            if (g_fade <= 0) HideOverlay();
            else { g_fadeStart = GetTickCount(); SetTimer(h, T_FADE, 16, NULL); }
        } else if (w == T_FADE) OnFade();
        return 0;
    case WM_TRAY:
        if (l == WM_RBUTTONUP) TrayMenu();
        else if (l == WM_LBUTTONDBLCLK) OpenSettings();
        return 0;
    case WM_OPEN: OpenSettings(); return 0;
    case WM_SETTINGCHANGE:
        if (IsColorSetChange(l)) RefreshDark();
        break;
    case WM_HOTKEY:
        cfg.enabled = !cfg.enabled; PI(L"Enabled", cfg.enabled); UpdateTip();
        if (!cfg.enabled) HideOverlay();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_ENABLE: cfg.enabled = !cfg.enabled; PI(L"Enabled", cfg.enabled); UpdateTip();
                         if (!cfg.enabled) HideOverlay();
                         break;
        case IDM_SETTINGS: OpenSettings(); break;
        case IDM_STARTUP: SetStartup(!IsStartup()); break;
        case IDM_EXIT: DestroyWindow(h); break;
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmd, int show) {
    hInst = hi;
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"WindowHighlighter.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(MAIN_CLASS, NULL);
        if (other) PostMessageW(other, WM_OPEN, 0, 0);
        return 0;
    }
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&ic);
    InitDark();
    InitIniPath();
    int first = GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES;
    LoadConfig();
    if (first) SaveConfig();

    WNDCLASSW wc = {0};
    wc.hInstance = hi; wc.lpfnWndProc = MainProc; wc.lpszClassName = MAIN_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"WHOverlay";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DlgProc; wc.lpszClassName = L"WHSettings";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = MakeIcon();
    RegisterClassW(&wc);

    wc.lpfnWndProc = PrevProc; wc.lpszClassName = L"WHPreview"; wc.hIcon = NULL;
    RegisterClassW(&wc);
    wc.lpfnWndProc = SliderProc; wc.lpszClassName = L"WHSlider";
    RegisterClassW(&wc);

    hMain = CreateWindowExW(0, MAIN_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    hOverlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"WHOverlay", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, hi, NULL);
    wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray();
    RegisterHotkey();
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEvt, 0, 0,
                                         WINEVENT_OUTOFCONTEXT);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (hDlg && IsDialogMessageW(hDlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWinEvent(hook);
    CloseHandle(mtx);
    return 0;
}
