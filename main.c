// PainelLeve - painel de controle leve na bandeja do Windows.
// Recursos: barra de tarefas transparente, medidor de velocidade, mixer de volume e bateria do mouse.
// C puro + Win32, sem dependencias. Compilar: build.cmd (usa zig cc).

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <objbase.h>
#include "mixer.h"
#include "battery.h"

#define APP_NAME      L"PainelLeve"
#define APP_TITLE     L"Painel Leve"
#define WM_TRAY       (WM_APP + 1)
#define WM_BATTERY    (WM_APP + 2)
#define TIMER_TICK    1
#define TICK_MS       1000
#define TIMER_HOVER   2
#define HOVER_MS      150

enum { ID_TRANSP = 100, ID_METER, ID_STARTUP, ID_EXIT, ID_MIXER, ID_BATTERY };

// ---- SetWindowCompositionAttribute (nao documentada) ----
typedef struct { int AccentState; int AccentFlags; DWORD GradientColor; int AnimationId; } ACCENT_POLICY;
typedef struct { int Attribute; void *Data; SIZE_T Size; } WCA_DATA;
typedef BOOL (WINAPI *PFN_SWCA)(HWND, WCA_DATA *);
enum { ACCENT_DISABLED = 0, ACCENT_TRANSPARENT = 2 };
#define WCA_ACCENT_POLICY 19

static PFN_SWCA g_swca;
static HINSTANCE g_hinst;
static HWND g_main, g_meter;
static HICON g_icon;
static HFONT g_font;
static UINT g_taskbarCreated;
static WCHAR g_ini[MAX_PATH], g_exe[MAX_PATH];
static BOOL g_transp, g_meterOn;
static int g_meterW, g_meterH, g_dpi = 96;
static WCHAR g_meterText[64] = L"\x2193 --   \x2191 --";

// medidor
static ULONG64 g_lastIn, g_lastOut;
static ULONGLONG g_lastTick;
static NET_IFINDEX g_ifIndex;
static int g_ifAge = 999;

// ---------------------------------------------------------------- utilidades
static int S(int px) { return MulDiv(px, g_dpi, 96); }

static void IniWriteInt(const WCHAR *key, int v) {
    WCHAR b[32]; _snwprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(L"geral", key, b, g_ini);
}
static int IniReadInt(const WCHAR *key, int def) { return GetPrivateProfileIntW(L"geral", key, def, g_ini); }

// Log de inicializacao (start.log ao lado do exe): ajuda a descobrir por que o app nao subiu no logon
static void StartLog(const WCHAR *msg) {
    WCHAR p[MAX_PATH]; wcscpy(p, g_exe); WCHAR *s = wcsrchr(p, L'\\'); if (!s) return; wcscpy(s + 1, L"start.log");
    FILE *f = _wfopen(p, L"a, ccs=UTF-8"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(f, L"[%04d-%02d-%02d %02d:%02d:%02d] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg);
    fclose(f);
}

// "Iniciar com o Windows" = atalho na pasta Inicializar do usuario (o Explorer o dispara depois de
// estar pronto; nao exige administrador, ao contrario de uma tarefa agendada de logon). A chave Run,
// usada antes, nao iniciava o app neste PC.
static void StartupLinkPath(WCHAR *out) {
    SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, out);
    wcscat(out, L"\\PainelLeve.lnk");
}
static BOOL StartupEnabled(void) { WCHAR p[MAX_PATH]; StartupLinkPath(p); return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static void SetStartup(BOOL on) {
    WCHAR p[MAX_PATH]; StartupLinkPath(p);
    if (!on) { DeleteFileW(p); }
    else {
        IShellLinkW *sl = NULL;
        if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
            IShellLinkW_SetPath(sl, g_exe);
            WCHAR dir[MAX_PATH]; wcscpy(dir, g_exe); *wcsrchr(dir, L'\\') = 0; IShellLinkW_SetWorkingDirectory(sl, dir);
            IShellLinkW_SetDescription(sl, L"Painel Leve");
            IPersistFile *pf = NULL;
            if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) { IPersistFile_Save(pf, p, TRUE); IPersistFile_Release(pf); }
            IShellLinkW_Release(sl);
        }
    }
    // remove a chave Run de versoes anteriores
    HKEY k; if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k)) { RegDeleteValueW(k, APP_NAME); RegCloseKey(k); }
}

// ---------------------------------------------------------------- icone da bandeja (desenhado em tempo de execucao)
// Estilo do Windows 11: glifo branco em linha. Usa o "equalizador" (tres sliders) da fonte Segoe Fluent Icons,
// desenhado em branco sobre preto; o brilho de cada pixel vira o canal alfa (antisserrilhado de verdade).
static HICON MakeGlyphIcon(int size) {
    BITMAPV5HEADER bi = {0};
    bi.bV5Size = sizeof bi; bi.bV5Width = size; bi.bV5Height = -size; bi.bV5Planes = 1;
    bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    DWORD *px; HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!color) { DeleteDC(dc); return NULL; }
    HGDIOBJ ob = SelectObject(dc, color);
    RECT r = { 0, 0, size, size }; HBRUSH blk = (HBRUSH)GetStockObject(BLACK_BRUSH); FillRect(dc, &r, blk);
    HFONT f = CreateFontW(-MulDiv(size, 15, 16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe Fluent Icons");
    HGDIOBJ of = SelectObject(dc, f);
    WCHAR face[LF_FACESIZE] = L""; GetTextFaceW(dc, LF_FACESIZE, face);
    BOOL ok = !_wcsicmp(face, L"Segoe Fluent Icons");
    if (!ok) {                                           // Windows 10: mesma glifo na fonte antiga
        SelectObject(dc, of); DeleteObject(f);
        f = CreateFontW(-MulDiv(size, 15, 16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe MDL2 Assets");
        SelectObject(dc, f); GetTextFaceW(dc, LF_FACESIZE, face); ok = !_wcsicmp(face, L"Segoe MDL2 Assets");
    }
    HICON h = NULL;
    if (ok) {
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, L"\xE9E9", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        GdiFlush();
        for (int i = 0; i < size * size; i++) {           // brilho -> alfa, cor branca pre-multiplicada
            BYTE a = (BYTE)(px[i] & 0xFF);
            px[i] = ((DWORD)a << 24) | ((DWORD)a << 16) | ((DWORD)a << 8) | a;
        }
        void *zeros = calloc((size_t)size * 4, size);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros); free(zeros);
        ICONINFO ii = { TRUE, 0, 0, mask, color };
        h = CreateIconIndirect(&ii);
        DeleteObject(mask);
    }
    SelectObject(dc, of); DeleteObject(f); SelectObject(dc, ob); DeleteObject(color); DeleteDC(dc);
    return h;
}

static HICON MakeIcon(int size) {
    HICON g = MakeGlyphIcon(size); if (g) return g;       // sem a fonte: cai no anel desenhado abaixo
    BITMAPV5HEADER bi = {0};
    bi.bV5Size = sizeof bi; bi.bV5Width = size; bi.bV5Height = -size; bi.bV5Planes = 1;
    bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    DWORD *px; HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, dc);
    void *zeros = calloc((size_t)size * 4, size);          // mascara zerada (a transparencia vem do canal alfa)
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros);
    free(zeros);
    double c = (size - 1) / 2.0, r = size / 2.0 - 0.5;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        double d = sqrt((x - c) * (x - c) + (y - c) * (y - c));
        double a = r - d + 0.5; if (a < 0) a = 0; if (a > 1) a = 1;        // borda suave
        BYTE A = (BYTE)(a * 255);
        // anel: dentro do circulo, furo central transparente (lembra um "painel/botao")
        double hole = r * 0.42, h = d - hole + 0.5; if (h < 0) h = 0; if (h > 1) h = 1;
        A = (BYTE)(A * h);
        BYTE R = (BYTE)(0x3C * A / 255), G = (BYTE)(0xA6 * A / 255), B = (BYTE)(0xFF * A / 255); // azul, pre-multiplicado
        px[y * size + x] = ((DWORD)A << 24) | (R << 16) | (G << 8) | B;
    }
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return h;
}

// ---------------------------------------------------------------- barra de tarefas transparente
// Duas camadas: (1) politica de acento do DWM na janela da barra; (2) a DLL PainelLeveTap,
// carregada no Explorer, que oculta o fundo XAML que o Windows 11 pinta por cima.
static const CLSID CLSID_Tap = { 0x7a1b2c3d, 0x4e5f, 0x4a6b, { 0x9c, 0x8d, 0x0e, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d } };
#define WM_TAP_SET (WM_APP + 1)
static WCHAR g_tapDll[MAX_PATH], g_tapClass[MAX_PATH + 32];
static int g_tapRetry;

static void ApplyAccent(HWND tb, int state) {
    if (!g_swca || !tb) return;
    ACCENT_POLICY p = { state, 2, 0x00000000, 0 };
    WCA_DATA d = { WCA_ACCENT_POLICY, &p, sizeof p };
    g_swca(tb, &d);
}
static HWND FindTap(void) { return FindWindowExW(HWND_MESSAGE, NULL, g_tapClass, NULL); }
static void InjectTap(void) {
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL); DWORD pid = 0;
    if (!tb || !GetWindowThreadProcessId(tb, &pid) || GetFileAttributesW(g_tapDll) == INVALID_FILE_ATTRIBUTES) return;
    HMODULE xaml = LoadLibraryW(L"Windows.UI.Xaml.dll"); if (!xaml) return;
    HRESULT (WINAPI *init)(PCWSTR, DWORD, PCWSTR, PCWSTR, CLSID, PCWSTR) = (void *)GetProcAddress(xaml, "InitializeXamlDiagnosticsEx");
    if (init) init(L"VisualDiagConnection1", pid, NULL, g_tapDll, CLSID_Tap, NULL);   // o nome do endpoint e fixo (outro da 0x80070490)
}
static void TapSet(BOOL on) { HWND t = FindTap(); if (t) PostMessageW(t, WM_TAP_SET, on, 0); }
static void ApplyTaskbars(int state) {
    ApplyAccent(FindWindowW(L"Shell_TrayWnd", NULL), state);
    HWND h = NULL;
    while ((h = FindWindowExW(NULL, h, L"Shell_SecondaryTrayWnd", NULL)) != NULL) ApplyAccent(h, state);
    if (state == ACCENT_DISABLED) { TapSet(FALSE); return; }
    if (!FindTap()) {                                   // ainda nao injetada (ou o Explorer reiniciou)
        if (g_tapRetry++ % 3 == 0) InjectTap();         // tenta a cada ~3 s para nao martelar o Explorer
    } else g_tapRetry = 0;
    TapSet(TRUE);
}

// ---------------------------------------------------------------- medidor de velocidade
static void FormatSpeed(double bps, WCHAR *out, int n) {
    if (bps >= 1e6) _snwprintf(out, n, L"%.1f Mb/s", bps / 1e6);
    else            _snwprintf(out, n, L"%.0f kb/s", bps / 1e3);
    for (WCHAR *p = out; *p; p++) if (*p == L'.') *p = L',';
}
static void UpdateSpeed(void) {
    if (++g_ifAge >= 10) {                           // reavalia a interface de saida a cada 10 s
        DWORD idx = 0; struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = 0x08080808; // 8.8.8.8
        if (GetBestInterfaceEx((struct sockaddr *)&sa, &idx) == NO_ERROR && idx != g_ifIndex) { g_ifIndex = idx; g_lastTick = 0; }
        g_ifAge = 0;
    }
    MIB_IF_ROW2 row = {0}; row.InterfaceIndex = g_ifIndex;
    if (!g_ifIndex || GetIfEntry2(&row) != NO_ERROR) { wcscpy(g_meterText, L"sem rede"); return; }
    ULONGLONG now = GetTickCount64();
    if (g_lastTick) {
        double dt = (now - g_lastTick) / 1000.0; if (dt <= 0) dt = 1;
        WCHAR dn[24], up[24];
        FormatSpeed((row.InOctets  - g_lastIn)  * 8.0 / dt, dn, 24);
        FormatSpeed((row.OutOctets - g_lastOut) * 8.0 / dt, up, 24);
        _snwprintf(g_meterText, 64, L"\x2193 %s   \x2191 %s", dn, up);
    }
    g_lastIn = row.InOctets; g_lastOut = row.OutOctets; g_lastTick = now;
}

static void SaveMeterPos(void) {
    RECT r; if (!g_meter || !GetWindowRect(g_meter, &r)) return;
    IniWriteInt(L"medidor_x", r.left); IniWriteInt(L"medidor_y", r.top);
}
static void DefaultMeterPos(int *x, int *y) {
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    *x = wa.right - g_meterW - S(12); *y = wa.bottom - g_meterH - S(8);
}

static LRESULT CALLBACK MeterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTCAPTION;                  // arrastavel por qualquer ponto
    case WM_EXITSIZEMOVE: SaveMeterPos(); return 0;
    case WM_NCRBUTTONUP: case WM_RBUTTONUP: PostMessageW(g_main, WM_TRAY, 0, WM_CONTEXTMENU); return 0;
    case WM_NCRBUTTONDOWN: return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp), of = SelectObject(mem, g_font);
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 24)); FillRect(mem, &r, bg); DeleteObject(bg);
        SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(235, 235, 235));
        DrawTextW(mem, g_meterText, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, of); SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps); return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowMeter(BOOL on) {
    if (on && !g_meter) {
        int x = IniReadInt(L"medidor_x", INT_MIN), y = IniReadInt(L"medidor_y", INT_MIN);
        RECT vs = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
        vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN); vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (x == INT_MIN || x < vs.left - g_meterW / 2 || x > vs.right - g_meterW / 2 || y < vs.top || y > vs.bottom - g_meterH / 2) DefaultMeterPos(&x, &y);
        g_meter = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                                  L"PainelLeveMeter", L"Velocidade", WS_POPUP, x, y, g_meterW, g_meterH, NULL, NULL, g_hinst, NULL);
        SetLayeredWindowAttributes(g_meter, 0, 215, LWA_ALPHA);
        int corner = 3; DwmSetWindowAttribute(g_meter, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof corner);
        g_lastTick = 0; g_ifAge = 999; UpdateSpeed();
        ShowWindow(g_meter, SW_SHOWNOACTIVATE);
    } else if (!on && g_meter) { DestroyWindow(g_meter); g_meter = NULL; }
}

// ---------------------------------------------------------------- bateria do mouse na barra de tarefas
// Janela filha "layered" da Shell_TrayWnd, logo a esquerda da area de notificacao, desenhada com alfa
// por pixel (UpdateLayeredWindow) para o texto se misturar com a barra transparente. Morre junto com o
// Explorer e e recriada. Exige o manifesto de compatibilidade Windows 8+ (PainelLeve.rc).
static HWND g_batt;
static void BatteryTip(const WCHAR *text);
static void TipShow(BOOL on);
static BOOL g_battOn;
static int g_battW;

static BOOL LightTaskbar(void) {
    DWORD v = 0, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz);
    return v != 0;
}

static void PlaceBattery(void) {
    if (!g_batt || !IsWindow(g_batt)) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tray || !g_battW) { ShowWindow(g_batt, SW_HIDE); return; }
    RECT tr, nr; GetWindowRect(tray, &tr);
    int right = tr.right;
    HWND notify = FindWindowExW(tray, NULL, L"TrayNotifyWnd", NULL);
    if (notify && GetWindowRect(notify, &nr) && nr.right > nr.left) right = nr.left;
    int x = right - tr.left - g_battW - S(4), h = tr.bottom - tr.top;
    RECT cur; GetWindowRect(g_batt, &cur);
    BOOL moved = cur.left - tr.left != x || cur.top != tr.top || cur.right - cur.left != g_battW || cur.bottom - cur.top != h;
    if (moved || GetWindow(tray, GW_CHILD) != g_batt || !IsWindowVisible(g_batt))   // acima do conteudo XAML da barra
        SetWindowPos(g_batt, HWND_TOP, x, 0, g_battW, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void DrawBattery(void) {
    if (!g_batt || !IsWindow(g_batt)) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL); if (!tray) return;
    BatteryInfo b;
    if (!Battery_Get(&b)) { g_battW = 0; PlaceBattery(); return; }
    RECT tr; GetWindowRect(tray, &tr);
    int h = tr.bottom - tr.top;
    BOOL light = LightTaskbar();
    COLORREF fg = light ? RGB(0, 0, 0) : RGB(255, 255, 255);
    COLORREF red = light ? RGB(0xC4, 0x2B, 0x1C) : RGB(0xFF, 0x6B, 0x6B);
    COLORREF green = light ? RGB(0x0F, 0x7B, 0x0F) : RGB(0x6C, 0xCB, 0x5F);
    COLORREF txtColor = (b.percent <= 15 && !b.charging) ? red : fg;
    BYTE opacity = b.stale ? 130 : 255;                 // mouse dormindo: ultimo valor, esmaecido

    struct { WCHAR t[8]; BOOL icon; COLORREF c; int x, w, gap; } seg[3];
    int ns = 0;
    wcscpy(seg[ns].t, L"\xE962"); seg[ns].icon = TRUE; seg[ns].c = txtColor; seg[ns].gap = S(5); ns++;   // mouse
    _snwprintf(seg[ns].t, 8, L"%d%%", b.percent); seg[ns].icon = FALSE; seg[ns].c = txtColor; seg[ns].gap = S(2); ns++;
    if (b.charging) { wcscpy(seg[ns].t, L"\xE945"); seg[ns].icon = TRUE; seg[ns].c = green; seg[ns].gap = 0; ns++; } // raio

    HDC dc = CreateCompatibleDC(NULL);
    HFONT ft = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT fi = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe Fluent Icons");
    int pad = S(6), x = pad;
    for (int i = 0; i < ns; i++) {
        SelectObject(dc, seg[i].icon ? fi : ft);
        SIZE sz; GetTextExtentPoint32W(dc, seg[i].t, (int)wcslen(seg[i].t), &sz);
        seg[i].x = x; seg[i].w = sz.cx; x += sz.cx + (i < ns - 1 ? seg[i].gap : 0);
    }
    int w = x + pad;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    DWORD *px; HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    ZeroMemory(px, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < ns; i++) {                      // branco sobre preto: o brilho vira cobertura (alfa)
        SelectObject(dc, seg[i].icon ? fi : ft);
        RECT r = { seg[i].x, 0, seg[i].x + seg[i].w, h };
        DrawTextW(dc, seg[i].t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOCLIP | DT_NOPREFIX);
    }
    GdiFlush();
    for (int y = 0; y < h; y++) for (int xx = 0; xx < w; xx++) {
        DWORD p = px[y * w + xx];
        int cov = max(max((int)(p & 0xFF), (int)(p >> 8 & 0xFF)), (int)(p >> 16 & 0xFF));
        COLORREF c = fg;
        for (int i = 0; i < ns; i++) if (xx >= seg[i].x - 1 && xx < seg[i].x + seg[i].w + 1) c = seg[i].c;
        int a = cov * opacity / 255;
        // pixel 100% transparente nao recebe clique; alfa minimo 1 deixa a janela "clicavel" por inteiro
        px[y * w + xx] = a ? ((DWORD)a << 24) | ((GetRValue(c) * a / 255) << 16) | ((GetGValue(c) * a / 255) << 8) | (GetBValue(c) * a / 255)
                           : 0x01000000;
    }
    POINT src = { 0, 0 }; SIZE size = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_batt, NULL, NULL, &size, dc, &src, 0, &bf, ULW_ALPHA);
    SelectObject(dc, ob); DeleteObject(bmp); DeleteObject(ft); DeleteObject(fi); DeleteDC(dc);

    WCHAR tip[128];
    _snwprintf(tip, 128, L"%s%s\nBateria: %d%%%s%s", wcsncmp(b.name, L"Logitech", 8) ? L"Logitech " : L"", b.name, b.percent,
               b.charging ? L" (carregando)" : L"", b.stale ? L" (em repouso)" : L"");
    BatteryTip(tip);
    g_battW = w;
    PlaceBattery();
}

static LRESULT CALLBACK BattProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONUP: Battery_Refresh(); return 0;                                   // clique: le agora
    case WM_RBUTTONUP: PostMessageW(g_main, WM_TRAY, 0, WM_CONTEXTMENU); return 0;
    case WM_NCDESTROY: TipShow(FALSE); if (h == g_batt) g_batt = NULL; break;
    }
    return DefWindowProcW(h, m, w, l);
}

// dica (tooltip) com o modelo do mouse ao passar o cursor. Tooltip "tracking" mostrado a mao:
// o modo automatico (TTF_SUBCLASS) nao aparecia sobre a barra de tarefas.
static HWND g_battTip;
static WCHAR g_tipText[160];
static BOOL g_tipShown;
static void BatteryTip(const WCHAR *text) {
    wcsncpy(g_tipText, text, 159); g_tipText[159] = 0;
    if (g_tipShown && g_battTip) {
        TTTOOLINFOW ti = {0}; ti.cbSize = sizeof ti; ti.hwnd = g_main; ti.uId = 1; ti.lpszText = g_tipText;
        SendMessageW(g_battTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}
static void TipShow(BOOL on) {
    TTTOOLINFOW ti = {0}; ti.cbSize = sizeof ti; ti.hwnd = g_main; ti.uId = 1;
    if (!on) { if (g_battTip && g_tipShown) SendMessageW(g_battTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti); g_tipShown = FALSE; return; }
    if (!g_battTip || !IsWindow(g_battTip)) {
        g_battTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_main, NULL, g_hinst, NULL);
        if (!g_battTip) return;
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE; ti.lpszText = g_tipText;
        SendMessageW(g_battTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_battTip, TTM_SETMAXTIPWIDTH, 0, S(400));
    }
    ti.lpszText = g_tipText;
    SendMessageW(g_battTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    RECT r; GetWindowRect(g_batt, &r);
    SendMessageW(g_battTip, TTM_TRACKPOSITION, 0, MAKELPARAM(r.left, r.top));
    SendMessageW(g_battTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    RECT tr; GetWindowRect(g_battTip, &tr);              // tamanho real ja com as duas linhas
    int tw = tr.right - tr.left, th = tr.bottom - tr.top;
    int x = (r.left + r.right) / 2 - tw / 2, y = r.top - th - S(6);
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST); MONITORINFO mi = { sizeof mi }; GetMonitorInfoW(mon, &mi);
    if (x + tw > mi.rcMonitor.right) x = mi.rcMonitor.right - tw - S(4);
    if (x < mi.rcMonitor.left) x = mi.rcMonitor.left;
    if (y < mi.rcMonitor.top) y = r.bottom + S(6);
    SetWindowPos(g_battTip, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    g_tipShown = TRUE;
}

// A barra do Windows 11 (XAML) captura o mouse: a janela filha nao recebe WM_MOUSEMOVE/HOVER.
// Por isso o cursor e consultado por timer enquanto o indicador esta ligado.
static void PollHover(void) {
    static int ticks;
    POINT pt; RECT r;
    BOOL in = g_batt && IsWindowVisible(g_batt) && GetCursorPos(&pt) && GetWindowRect(g_batt, &r) && PtInRect(&r, pt);
    if (!in) { ticks = 0; if (g_tipShown) TipShow(FALSE); return; }
    if (++ticks == 400 / HOVER_MS) TipShow(TRUE);
}

static void EnsureBattery(void) {
    if (!g_battOn) { if (g_batt) { DestroyWindow(g_batt); g_batt = NULL; } return; }
    if (g_batt && IsWindow(g_batt)) { PlaceBattery(); return; }
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL); if (!tray) return;
    g_batt = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PainelLeveBattery", L"", WS_CHILD | WS_CLIPSIBLINGS,
                             0, 0, 1, 1, tray, NULL, g_hinst, NULL);
    if (!g_batt) { StartLog(L"falha ao criar o indicador de bateria"); return; }
    g_battW = 0;
    DrawBattery();
}

static void ShowBattery(BOOL on) {
    if (on) { Battery_Start(g_main, WM_BATTERY); EnsureBattery(); Battery_Refresh(); }
    else { if (g_batt) DestroyWindow(g_batt); g_batt = NULL; }
}

// ---------------------------------------------------------------- bandeja e janela principal (oculta)
static void TrayAdd(void) {
    NOTIFYICONDATAW n = { sizeof n }; n.hWnd = g_main; n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; n.uCallbackMessage = WM_TRAY; n.hIcon = g_icon;
    wcscpy(n.szTip, APP_TITLE);
    Shell_NotifyIconW(NIM_ADD, &n); n.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &n);
}
static void TrayRemove(void) { NOTIFYICONDATAW n = { sizeof n }; n.hWnd = g_main; n.uID = 1; Shell_NotifyIconW(NIM_DELETE, &n); }

static void UpdateTimer(void) {
    if (g_transp || g_meterOn || g_battOn) SetTimer(g_main, TIMER_TICK, TICK_MS, NULL); else KillTimer(g_main, TIMER_TICK);
    if (g_battOn) SetTimer(g_main, TIMER_HOVER, HOVER_MS, NULL); else { KillTimer(g_main, TIMER_HOVER); TipShow(FALSE); }
}

static void ShowMenu(void) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_MIXER, L"Mixer de volume");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_transp ? MF_CHECKED : 0), ID_TRANSP, L"Barra de tarefas transparente");
    AppendMenuW(m, MF_STRING | (g_meterOn ? MF_CHECKED : 0), ID_METER, L"Medidor de velocidade");
    AppendMenuW(m, MF_STRING | (g_battOn ? MF_CHECKED : 0), ID_BATTERY, L"Bateria do mouse na barra");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Iniciar com o Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Sair");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_main);                        // necessario para o menu fechar ao clicar fora
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated) {                        // Explorer reiniciou: recoloca o icone e reaplica
        TrayAdd(); if (g_transp) ApplyTaskbars(ACCENT_TRANSPARENT); g_batt = NULL; EnsureBattery(); return 0;
    }
    switch (m) {
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP) Mixer_Toggle();          // clique esquerdo: mixer de volume
        else if (LOWORD(l) == WM_CONTEXTMENU) ShowMenu();        // clique direito: menu
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_TRANSP:  g_transp = !g_transp; IniWriteInt(L"transparente", g_transp);
                         ApplyTaskbars(g_transp ? ACCENT_TRANSPARENT : ACCENT_DISABLED); UpdateTimer(); break;
        case ID_METER:   g_meterOn = !g_meterOn; IniWriteInt(L"medidor", g_meterOn); ShowMeter(g_meterOn); UpdateTimer(); break;
        case ID_STARTUP: SetStartup(!StartupEnabled()); break;
        case ID_BATTERY: g_battOn = !g_battOn; IniWriteInt(L"bateria", g_battOn); ShowBattery(g_battOn); UpdateTimer(); break;
        case ID_MIXER:   Mixer_Toggle(); break;
        case ID_EXIT:    DestroyWindow(h); break;
        }
        return 0;
    case WM_TIMER:
        if (w == TIMER_HOVER) { PollHover(); return 0; }
        if (g_transp) ApplyTaskbars(ACCENT_TRANSPARENT);   // o Explorer as vezes volta ao normal; reaplicar e barato
        if (g_meter) { UpdateSpeed(); InvalidateRect(g_meter, NULL, FALSE); }
        if (g_battOn) EnsureBattery();                  // recria se o Explorer reiniciou; mantem a posicao
        return 0;
    case WM_BATTERY: DrawBattery(); return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE:
        if (g_transp) ApplyTaskbars(ACCENT_TRANSPARENT);
        if (g_batt) DrawBattery();                      // tema claro/escuro, DPI, tamanho da barra
        return 0;
    case WM_DESTROY:
        KillTimer(h, TIMER_TICK); Mixer_Close(); ShowMeter(FALSE); ShowBattery(FALSE); Battery_Stop(); TrayRemove();
        if (g_transp) ApplyTaskbars(ACCENT_DISABLED);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    g_hinst = hi;
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\PainelLeve.Instancia");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { StartLog(L"ja estava rodando; saindo"); return 0; }
    StartLog(L"iniciando");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);           // Core Audio (mixer)
    // No logon o app pode subir antes da barra de tarefas existir: espera ate 60 s por ela
    for (int i = 0; i < 120 && !FindWindowW(L"Shell_TrayWnd", NULL); i++) { if (i == 0) StartLog(L"barra de tarefas ainda nao existe; esperando"); Sleep(500); }
    StartLog(FindWindowW(L"Shell_TrayWnd", NULL) ? L"barra de tarefas encontrada" : L"barra de tarefas nao apareceu em 60 s; seguindo assim mesmo");

    // DPI por monitor (sem manifesto): carrega em tempo de execucao
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *setCtx)(HANDLE) = (void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (setCtx) setCtx((HANDLE)-4 /*PER_MONITOR_AWARE_V2*/);
    g_swca = (PFN_SWCA)GetProcAddress(u32, "SetWindowCompositionAttribute");
    UINT (WINAPI *getDpi)(void) = (void *)GetProcAddress(u32, "GetDpiForSystem");
    if (getDpi) g_dpi = getDpi();

    wcscpy(g_ini, g_exe); WCHAR *dot = wcsrchr(g_ini, L'\\'); wcscpy(dot + 1, L"config.ini");
    // DLL do Explorer: nome configuravel (tap_dll no config.ini) para permitir trocar de versao sem reiniciar o Explorer
    WCHAR tapName[MAX_PATH]; GetPrivateProfileStringW(L"geral", L"tap_dll", L"PainelLeveTap.dll", tapName, MAX_PATH, g_ini);
    wcscpy(g_tapDll, g_exe); dot = wcsrchr(g_tapDll, L'\\'); wcscpy(dot + 1, tapName);
    _snwprintf(g_tapClass, MAX_PATH + 32, L"PainelLeveTapWnd:%s", tapName);

    g_icon = MakeIcon(GetSystemMetrics(SM_CXSMICON));
    g_font = CreateFontW(-S(12), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_meterW = S(190); g_meterH = S(24);
    Mixer_Init(hi, g_font, g_dpi);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {0}; wc.lpfnWndProc = MainProc; wc.hInstance = hi; wc.lpszClassName = L"PainelLeveMain"; RegisterClassW(&wc);
    WNDCLASSW mc = {0}; mc.lpfnWndProc = MeterProc; mc.hInstance = hi; mc.lpszClassName = L"PainelLeveMeter";
    mc.hCursor = LoadCursorW(NULL, IDC_ARROW); RegisterClassW(&mc);
    WNDCLASSW bc = {0}; bc.lpfnWndProc = BattProc; bc.hInstance = hi; bc.lpszClassName = L"PainelLeveBattery";
    bc.hCursor = LoadCursorW(NULL, IDC_ARROW); RegisterClassW(&bc);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_WIN95_CLASSES }; InitCommonControlsEx(&icc);

    // janela top-level oculta (nao message-only): recebe o broadcast "TaskbarCreated" e serve de dona do menu
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, L"PainelLeveMain", APP_TITLE, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    TrayAdd();

    g_transp  = IniReadInt(L"transparente", 1);
    g_meterOn = IniReadInt(L"medidor", 1);
    if (g_transp) ApplyTaskbars(ACCENT_TRANSPARENT);
    g_battOn  = IniReadInt(L"bateria", 1);
    ShowMeter(g_meterOn);
    if (g_battOn) ShowBattery(TRUE);
    UpdateTimer();

    StartLog(L"pronto (icone na bandeja)");
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    StartLog(L"encerrando normalmente");
    DeleteObject(g_font); DestroyIcon(g_icon); CloseHandle(mtx);
    return 0;
}
