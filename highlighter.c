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
#include <math.h>
#include <wchar.h>

enum { MODE_BORDER, MODE_TINT, MODE_BOTH };

typedef struct {
    int enabled, mode, opacity, tintOpacity, thickness, radius, holdMs, fadeMs;
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
enum { IDC_ENABLE = 101, IDC_MODE, IDC_SWATCH, IDC_PICK, IDC_OPACITY, IDC_TINT, IDC_THICK,
       IDC_RADIUS, IDC_HOLD, IDC_FADE, IDC_HOTKEY, IDC_HKCLEAR, IDC_IGNORE, IDC_STARTUP,
       IDC_PREVIEW, IDC_APPLY };

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
    c->thickness = 4; c->radius = 8; c->holdMs = 350; c->fadeMs = 250;
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
    c->radius = Clampi(c->radius, 0, 100);
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
    cfg.radius = GI(L"Radius", d.radius);
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
    PI(L"TintOpacity", cfg.tintOpacity); PI(L"Thickness", cfg.thickness); PI(L"Radius", cfg.radius);
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
static void Render(DWORD *px, int w, int h, const Config *c, float r) {
    float t = (float)c->thickness;
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
            float a = co * (Ab * (1 - ci) + At * ci);
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

static void ShowHighlight(HWND target, const Config *c) {
    RECT r;
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)))
        GetWindowRect(target, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 8 || h < 8) return;
    HideOverlay();

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

    float radius = IsZoomed(target) ? 0.f : (float)c->radius;
    Render((DWORD *)bits, w, h, c, radius);

    g_hold = c->holdMs; g_fade = c->fadeMs;
    SetWindowPos(hOverlay, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    POINT dst = { r.left, r.top }, src = {0, 0};
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
    Render((DWORD *)bits, s, s, &t, s / 5.f);
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

// ---------------------------------------------------------------- settings dialog
static int g_dpi = 96;
static HFONT g_font;
static COLORREF dlgColor;
static HWND hSwatch;
static COLORREF custColors[16];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND Mk(LPCWSTR cls, LPCWSTR text, DWORD style, DWORD ex, int x, int y, int w, int h, int id) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                           hDlg, (HMENU)(INT_PTR)id, hInst, NULL);
}
static void Label(LPCWSTR t, int y) { Mk(L"STATIC", t, SS_LEFT | SS_CENTERIMAGE, 0, 12, y, 150, 22, -1); }
static void NumRow(LPCWSTR t, int id, int y) {
    Label(t, y);
    Mk(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 170, y, 70, 22, id);
}

static BOOL CALLBACK SetFont(HWND h, LPARAM f) { SendMessageW(h, WM_SETFONT, f, TRUE); return TRUE; }

static int GetNum(int id) { return (int)GetDlgItemInt(hDlg, id, NULL, FALSE); }

static void ReadDialog(Config *c) {
    c->enabled = IsDlgButtonChecked(hDlg, IDC_ENABLE) == BST_CHECKED;
    c->mode = (int)SendDlgItemMessageW(hDlg, IDC_MODE, CB_GETCURSEL, 0, 0);
    c->color = dlgColor;
    c->opacity = GetNum(IDC_OPACITY); c->tintOpacity = GetNum(IDC_TINT);
    c->thickness = GetNum(IDC_THICK); c->radius = GetNum(IDC_RADIUS);
    c->holdMs = GetNum(IDC_HOLD); c->fadeMs = GetNum(IDC_FADE);
    c->hotkey = LOWORD(SendDlgItemMessageW(hDlg, IDC_HOTKEY, HKM_GETHOTKEY, 0, 0));
    GetDlgItemTextW(hDlg, IDC_IGNORE, c->ignore, 256);
    Sanitize(c);
}

static void PutDialog(const Config *c) {
    CheckDlgButton(hDlg, IDC_ENABLE, c->enabled ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(hDlg, IDC_MODE, CB_SETCURSEL, c->mode, 0);
    dlgColor = c->color;
    SetDlgItemInt(hDlg, IDC_OPACITY, c->opacity, FALSE); SetDlgItemInt(hDlg, IDC_TINT, c->tintOpacity, FALSE);
    SetDlgItemInt(hDlg, IDC_THICK, c->thickness, FALSE); SetDlgItemInt(hDlg, IDC_RADIUS, c->radius, FALSE);
    SetDlgItemInt(hDlg, IDC_HOLD, c->holdMs, FALSE); SetDlgItemInt(hDlg, IDC_FADE, c->fadeMs, FALSE);
    SendDlgItemMessageW(hDlg, IDC_HOTKEY, HKM_SETHOTKEY, c->hotkey, 0);
    SetDlgItemTextW(hDlg, IDC_IGNORE, c->ignore);
    CheckDlgButton(hDlg, IDC_STARTUP, IsStartup() ? BST_CHECKED : BST_UNCHECKED);
    InvalidateRect(hSwatch, NULL, TRUE);
}

static void ApplyDialog(void) {
    ReadDialog(&cfg);
    SaveConfig();
    SetStartup(IsDlgButtonChecked(hDlg, IDC_STARTUP) == BST_CHECKED);
    RegisterHotkey();
    UpdateTip();
    PutDialog(&cfg);    // show clamped values
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        hDlg = h;
        int y = 12;
        Mk(L"BUTTON", L"Highlight when window focus changes", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 12, y, 330, 22, IDC_ENABLE); y += 30;
        Label(L"Style", y);
        HWND cb = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, 170, y, 172, 120, IDC_MODE);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Border");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Tint");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Border + tint"); y += 30;
        Label(L"Color", y);
        hSwatch = Mk(L"STATIC", L"", SS_NOTIFY | WS_BORDER, 0, 170, y, 40, 22, IDC_SWATCH);
        Mk(L"BUTTON", L"Choose...", BS_PUSHBUTTON | WS_TABSTOP, 0, 218, y, 90, 22, IDC_PICK); y += 30;
        NumRow(L"Border opacity (%)", IDC_OPACITY, y); y += 30;
        NumRow(L"Tint opacity (%)", IDC_TINT, y); y += 30;
        NumRow(L"Border thickness (px)", IDC_THICK, y); y += 30;
        NumRow(L"Corner radius (px)", IDC_RADIUS, y); y += 30;
        NumRow(L"Show duration (ms)", IDC_HOLD, y); y += 30;
        NumRow(L"Fade-out time (ms)", IDC_FADE, y); y += 30;
        Label(L"Toggle hotkey", y);
        Mk(HOTKEY_CLASSW, L"", WS_TABSTOP, WS_EX_CLIENTEDGE, 170, y, 130, 22, IDC_HOTKEY);
        Mk(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, 0, 306, y, 36, 22, IDC_HKCLEAR); y += 30;
        Mk(L"STATIC", L"Ignored programs (exe names separated by ;)", SS_LEFT, 0, 12, y, 330, 18, -1); y += 20;
        Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 12, y, 330, 22, IDC_IGNORE); y += 30;
        Mk(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 12, y, 330, 22, IDC_STARTUP); y += 34;
        Mk(L"BUTTON", L"Preview", BS_PUSHBUTTON | WS_TABSTOP, 0, 12, y, 72, 26, IDC_PREVIEW);
        Mk(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 120, y, 66, 26, IDOK);
        Mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, 192, y, 66, 26, IDCANCEL);
        Mk(L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 0, 264, y, 78, 26, IDC_APPLY); y += 38;

        g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
        EnumChildWindows(h, SetFont, (LPARAM)g_font);
        PutDialog(&cfg);

        RECT rc = { 0, 0, S(354), S(y) };
        AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU, FALSE, 0, g_dpi);
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        SetWindowPos(h, NULL, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                     ww, wh, SWP_NOZORDER);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == hSwatch) {
            SetDCBrushColor((HDC)w, dlgColor);
            return (LRESULT)GetStockObject(DC_BRUSH);
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_SWATCH: case IDC_PICK: {
            CHOOSECOLORW cc = { sizeof cc, h, NULL, dlgColor, custColors, CC_RGBINIT | CC_FULLOPEN };
            if (ChooseColorW(&cc)) { dlgColor = cc.rgbResult; InvalidateRect(hSwatch, NULL, TRUE); }
            return 0;
        }
        case IDC_HKCLEAR: SendDlgItemMessageW(h, IDC_HOTKEY, HKM_SETHOTKEY, 0, 0); return 0;
        case IDC_PREVIEW: { Config t = cfg; ReadDialog(&t); ShowHighlight(h, &t); return 0; }
        case IDC_APPLY: ApplyDialog(); return 0;
        case IDOK: ApplyDialog(); DestroyWindow(h); return 0;
        case IDCANCEL: DestroyWindow(h); return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        DeleteObject(g_font); g_font = NULL; hDlg = NULL;
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
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
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
