// PainelLeve - painel de controle leve na bandeja do Windows.
// Recursos: barra de tarefas transparente e medidor de velocidade da internet.
// C puro + Win32, sem dependencias. Compilar: build.cmd (usa zig cc).

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
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

#define APP_NAME      L"PainelLeve"
#define APP_TITLE     L"Painel Leve"
#define WM_TRAY       (WM_APP + 1)
#define TIMER_TICK    1
#define TICK_MS       1000

enum { ID_TRANSP = 100, ID_METER, ID_STARTUP, ID_EXIT, ID_MIXER };

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

// "Iniciar com o Windows" = tarefa agendada no logon, com atraso de 15 s (a chave Run dispara cedo
// demais, antes da barra de tarefas existir, e se mostrou pouco confiavel neste PC).
static BOOL RunHidden(const WCHAR *cmdline, DWORD *exitCode) {
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    WCHAR buf[2048]; wcsncpy(buf, cmdline, 2047); buf[2047] = 0;
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    WaitForSingleObject(pi.hProcess, 15000); GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return TRUE;
}
static BOOL StartupEnabled(void) {
    DWORD code = 1;
    return RunHidden(L"schtasks.exe /Query /TN \"PainelLeve\"", &code) && code == 0;
}
static void SetStartup(BOOL on) {
    DWORD code;
    if (on) {
        // cria/atualiza a tarefa via XML (unico jeito de ter atraso no logon sem a interface grafica)
        WCHAR xmlPath[MAX_PATH]; GetTempPathW(MAX_PATH, xmlPath); wcscat(xmlPath, L"PainelLeve-task.xml");
        FILE *f = _wfopen(xmlPath, L"w, ccs=UTF-16LE");
        if (!f) return;
        fwprintf(f,
            L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
            L"  <Triggers><LogonTrigger><Enabled>true</Enabled><Delay>PT15S</Delay></LogonTrigger></Triggers>\n"
            L"  <Principals><Principal id=\"u\"><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\n"
            L"  <Settings><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
            L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
            L"    <StartWhenAvailable>true</StartWhenAvailable><Priority>7</Priority></Settings>\n"
            L"  <Actions Context=\"u\"><Exec><Command>%s</Command></Exec></Actions>\n"
            L"</Task>\n", g_exe);
        fclose(f);
        WCHAR cmd[MAX_PATH * 2]; _snwprintf(cmd, MAX_PATH * 2, L"schtasks.exe /Create /F /TN \"PainelLeve\" /XML \"%s\"", xmlPath);
        RunHidden(cmd, &code); DeleteFileW(xmlPath);
    } else RunHidden(L"schtasks.exe /Delete /F /TN \"PainelLeve\"", &code);
    // remove a chave Run antiga, se existir (versoes anteriores)
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

// ---------------------------------------------------------------- bandeja e janela principal (oculta)
static void TrayAdd(void) {
    NOTIFYICONDATAW n = { sizeof n }; n.hWnd = g_main; n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; n.uCallbackMessage = WM_TRAY; n.hIcon = g_icon;
    wcscpy(n.szTip, APP_TITLE);
    Shell_NotifyIconW(NIM_ADD, &n); n.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &n);
}
static void TrayRemove(void) { NOTIFYICONDATAW n = { sizeof n }; n.hWnd = g_main; n.uID = 1; Shell_NotifyIconW(NIM_DELETE, &n); }

static void UpdateTimer(void) {
    if (g_transp || g_meterOn) SetTimer(g_main, TIMER_TICK, TICK_MS, NULL); else KillTimer(g_main, TIMER_TICK);
}

static void ShowMenu(void) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_MIXER, L"Mixer de volume");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_transp ? MF_CHECKED : 0), ID_TRANSP, L"Barra de tarefas transparente");
    AppendMenuW(m, MF_STRING | (g_meterOn ? MF_CHECKED : 0), ID_METER, L"Medidor de velocidade");
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
        TrayAdd(); if (g_transp) ApplyTaskbars(ACCENT_TRANSPARENT); return 0;
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
        case ID_MIXER:   Mixer_Toggle(); break;
        case ID_EXIT:    DestroyWindow(h); break;
        }
        return 0;
    case WM_TIMER:
        if (g_transp) ApplyTaskbars(ACCENT_TRANSPARENT);   // o Explorer as vezes volta ao normal; reaplicar e barato
        if (g_meter) { UpdateSpeed(); InvalidateRect(g_meter, NULL, FALSE); }
        return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE:
        if (g_transp) ApplyTaskbars(ACCENT_TRANSPARENT);
        return 0;
    case WM_DESTROY:
        KillTimer(h, TIMER_TICK); Mixer_Close(); ShowMeter(FALSE); TrayRemove();
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

    // janela top-level oculta (nao message-only): recebe o broadcast "TaskbarCreated" e serve de dona do menu
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, L"PainelLeveMain", APP_TITLE, WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    TrayAdd();

    g_transp  = IniReadInt(L"transparente", 1);
    g_meterOn = IniReadInt(L"medidor", 1);
    if (g_transp) ApplyTaskbars(ACCENT_TRANSPARENT);
    ShowMeter(g_meterOn);
    UpdateTimer();

    StartLog(L"pronto (icone na bandeja)");
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    StartLog(L"encerrando normalmente");
    DeleteObject(g_font); DestroyIcon(g_icon); CloseHandle(mtx);
    return 0;
}
