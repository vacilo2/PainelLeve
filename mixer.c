// Mixer de volume por aplicativo (estilo EarTrumpet) em C puro + Core Audio.
// Lista cada dispositivo de saida ativo (volume principal) e, abaixo, um slider por
// aplicativo com sessao de audio nele. Arrastar/rolar muda o volume; clicar no icone silencia.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmsystem.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>
#include "mixer.h"

// IAudioMeterInformation: o cabecalho do mingw so declara o nome; definimos a interface aqui (ordem dos metodos do SDK)
typedef struct IAudioMeterInformationVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAudioMeterInformation *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IAudioMeterInformation *);
    ULONG   (STDMETHODCALLTYPE *Release)(IAudioMeterInformation *);
    HRESULT (STDMETHODCALLTYPE *GetPeakValue)(IAudioMeterInformation *, float *);
    HRESULT (STDMETHODCALLTYPE *GetMeteringChannelCount)(IAudioMeterInformation *, UINT *);
    HRESULT (STDMETHODCALLTYPE *GetChannelsPeakValues)(IAudioMeterInformation *, UINT, float *);
    HRESULT (STDMETHODCALLTYPE *QueryHardwareSupport)(IAudioMeterInformation *, DWORD *);
} IAudioMeterInformationVtbl;
struct IAudioMeterInformation { CONST_VTBL IAudioMeterInformationVtbl *lpVtbl; };
#define IAudioMeterInformation_Release(T)         (T)->lpVtbl->Release(T)
#define IAudioMeterInformation_GetPeakValue(T, p) (T)->lpVtbl->GetPeakValue(T, p)
DEFINE_GUID(IID_IAudioMeterInformation, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64);

#define MAX_ROWS   64
#define MAX_SV     16
#define TIMER_REFRESH 1
#define TIMER_ANIM    2
#define TIMER_BEEP    3                  // toca o som do sistema um pouco depois da ultima mudanca pela roda
#define TIMER_PEAK    4                  // medidor de nivel (20x por segundo, so com o painel aberto)
#define ANIM_OPEN_MS  180
#define ANIM_CLOSE_MS 110

typedef struct {
    int kind;                            // 0 = dispositivo, 1 = aplicativo
    WCHAR name[96];
    WCHAR key[MAX_PATH];                 // chave de agrupamento (exe) para unir sessoes do mesmo app
    float vol; BOOL mute;
    HICON icon;
    BOOL glyph;                          // sem icone: desenha um glifo da fonte Segoe Fluent Icons
    WCHAR glyphChar;                     // qual glifo (alto-falante, fone, headset, HDMI...)
    IAudioEndpointVolume *ev;            // dispositivo
    ISimpleAudioVolume *sv[MAX_SV]; int nsv;   // aplicativo (uma ou mais sessoes)
    IAudioMeterInformation *meter[MAX_SV]; int nmeter;   // nivel de pico (dispositivo: 1; app: por sessao)
    float peak;                          // ultimo nivel lido (0..1)
    int y, h;                            // layout (coordenadas do conteudo, antes do scroll)
    int dev;                             // indice do dispositivo a que a linha pertence (0 = padrao)
    BOOL hidden;                         // escondida no modo "so o dispositivo padrao"
} Row;
static BOOL g_showAll = FALSE;           // seta no cabecalho: FALSE = so o dispositivo padrao (como o EarTrumpet)
static int g_devCount;
static int g_x, g_finalY;                // posicao final do painel
static ULONGLONG g_animStart, g_lastClose;   // animacao em curso / quando o painel fechou pela ultima vez
static int g_animDir;                    // +1 abrindo, -1 fechando, 0 parado
static void StartAnim(int dir);
// Som de "amostra" do volume: o mesmo som padrao do sistema que o controle de volume do Windows toca
static void PlayVolumeSound(void) { PlaySoundW((LPCWSTR)SND_ALIAS_SYSTEMDEFAULT, NULL, SND_ALIAS_ID | SND_ASYNC | SND_NODEFAULT); }

static HINSTANCE g_hinst; static HFONT g_font, g_fontBold, g_fontSmall, g_fontIcon; static int g_dpi = 96;
static HWND g_wnd; static Row g_rows[MAX_ROWS]; static int g_n;
static int g_W, g_contentH, g_viewH, g_scroll;
static int g_drag = -1;                  // linha cujo slider esta sendo arrastado
static int g_pendingRow = -1;            // linha com volume alterado na tela mas ainda nao enviado ao Windows
static CRITICAL_SECTION g_cs; static HANDLE g_evt, g_thread; static volatile BOOL g_quit;
static float g_pendingVol; static ULONGLONG g_lastSetTick;
static COLORREF g_accent = RGB(0, 120, 212);

static int S(int px) { return MulDiv(px, g_dpi, 96); }
#define PAD        S(12)
#define ROW_APP    S(48)
#define ROW_DEV    S(60)
#define ICON_SZ    S(26)
#define SLIDER_X   S(52)
#define VALUE_W    S(44)
#define THUMB_R    S(7)
#define GAP_DEV    S(8)

// ---------------------------------------------------------------- enumeracao Core Audio
static void FreeRows(void) {
    EnterCriticalSection(&g_cs); g_pendingRow = -1;     // a thread auxiliar nao pode ler linhas sendo liberadas
    for (int i = 0; i < g_n; i++) {
        Row *r = &g_rows[i];
        if (r->ev) IAudioEndpointVolume_Release(r->ev);
        for (int j = 0; j < r->nsv; j++) ISimpleAudioVolume_Release(r->sv[j]);
        for (int j = 0; j < r->nmeter; j++) IAudioMeterInformation_Release(r->meter[j]);
        if (r->icon) DestroyIcon(r->icon);
    }
    g_n = 0;
    LeaveCriticalSection(&g_cs);
}

// Nome amigavel do executavel: FileDescription dos recursos de versao (o que o Gerenciador de Tarefas mostra)
static BOOL FileDescription(const WCHAR *path, WCHAR *out, int n) {
    DWORD dummy, sz = GetFileVersionInfoSizeW(path, &dummy); if (!sz) return FALSE;
    void *buf = malloc(sz); BOOL ok = FALSE;
    if (buf && GetFileVersionInfoW(path, 0, sz, buf)) {
        struct { WORD lang, cp; } *tr = NULL; UINT tl = 0;
        if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&tr, &tl) && tl >= 4) {
            WCHAR sub[64]; _snwprintf(sub, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
            WCHAR *desc = NULL; UINT dl = 0;
            if (VerQueryValueW(buf, sub, (void **)&desc, &dl) && desc && desc[0]) { wcsncpy(out, desc, n - 1); out[n - 1] = 0; ok = TRUE; }
        }
    }
    free(buf); return ok;
}

static HICON IconForProcess(DWORD pid, WCHAR *exeName, int n, WCHAR *friendly, int fn) {
    exeName[0] = 0; friendly[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return NULL;
    WCHAR path[MAX_PATH]; DWORD len = MAX_PATH; HICON icon = NULL;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        const WCHAR *base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        wcsncpy(exeName, base, n - 1); exeName[n - 1] = 0;
        FileDescription(path, friendly, fn);
        // pede ao proprio arquivo um icone no tamanho exato (fica nitido); se nao tiver, cai no padrao do shell
        if (PrivateExtractIconsW(path, 0, ICON_SZ, ICON_SZ, &icon, NULL, 1, 0) == 0 || !icon) {
            SHFILEINFOW sfi = {0}; icon = NULL;
            if (SHGetFileInfoW(path, 0, &sfi, sizeof sfi, SHGFI_ICON | SHGFI_LARGEICON)) icon = sfi.hIcon;
        }
    }
    CloseHandle(h);
    return icon;
}

static void PrettyName(const WCHAR *exe, WCHAR *out, int n) {   // "discord.exe" -> "Discord"
    wcsncpy(out, exe, n - 1); out[n - 1] = 0;
    WCHAR *dot = wcsrchr(out, L'.'); if (dot) *dot = 0;
    if (out[0] >= L'a' && out[0] <= L'z') out[0] -= 32;
}

static int CmpRows(const void *a, const void *b) { return _wcsicmp(((const Row *)a)->name, ((const Row *)b)->name); }

static void AddDevice(IMMDevice *dev) {
    if (g_n >= MAX_ROWS) return;
    int devIdx = g_devCount++;
    Row *d = &g_rows[g_n]; memset(d, 0, sizeof *d); d->kind = 0; d->dev = devIdx; wcscpy(d->name, L"Dispositivo");
    d->glyphChar = 0xE767;               // volume (generico)
    IPropertyStore *ps = NULL;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) { wcsncpy(d->name, pv.pwszVal, 95); d->name[95] = 0; }
        PropVariantClear(&pv);
        // formato declarado pelo driver (nao e deteccao fisica): escolhe o icone
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_AudioEndpoint_FormFactor, &pv)) && pv.vt == VT_UI4) {
            switch (pv.ulVal) {
            case Headphones:                 d->glyphChar = 0xE7F6; break;   // fone
            case Headset:                    d->glyphChar = 0xE95B; break;   // headset (fone com microfone)
            case Speakers: case LineLevel:   d->glyphChar = 0xE7F5; break;   // alto-falantes
            case DigitalAudioDisplayDevice:  d->glyphChar = 0xE7F4; break;   // HDMI / DisplayPort (monitor ou TV)
            case SPDIF:                      d->glyphChar = 0xE767; break;   // saida digital
            default: break;
            }
        }
        PropVariantClear(&pv); IPropertyStore_Release(ps);
    }
    if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&d->ev))) {
        IAudioEndpointVolume_GetMasterVolumeLevelScalar(d->ev, &d->vol);
        IAudioEndpointVolume_GetMute(d->ev, &d->mute);
    }
    d->glyph = TRUE;
    if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioMeterInformation, CLSCTX_ALL, NULL, (void **)&d->meter[0]))) d->nmeter = 1;
    int first = ++g_n;

    IAudioSessionManager2 *sm = NULL;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&sm))) return;
    IAudioSessionEnumerator *se = NULL; int cnt = 0;
    if (SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(sm, &se))) {
        IAudioSessionEnumerator_GetCount(se, &cnt);
        for (int i = 0; i < cnt; i++) {
            IAudioSessionControl *sc = NULL; IAudioSessionControl2 *sc2 = NULL; ISimpleAudioVolume *sv = NULL;
            if (FAILED(IAudioSessionEnumerator_GetSession(se, i, &sc))) continue;
            AudioSessionState st = AudioSessionStateExpired; IAudioSessionControl_GetState(sc, &st);
            if (st != AudioSessionStateExpired && SUCCEEDED(IAudioSessionControl_QueryInterface(sc, &IID_IAudioSessionControl2, (void **)&sc2))) {
                DWORD pid = 0; IAudioSessionControl2_GetProcessId(sc2, &pid);
                BOOL sys = IAudioSessionControl2_IsSystemSoundsSession(sc2) == S_OK;
                if (!sys && pid == GetCurrentProcessId()) { IAudioSessionControl2_Release(sc2); IAudioSessionControl_Release(sc); continue; }   // nosso proprio bipe
                WCHAR exe[MAX_PATH] = L"", friendly[96] = L""; HICON icon = NULL;
                if (sys) wcscpy(exe, L"*system*"); else icon = IconForProcess(pid, exe, MAX_PATH, friendly, 96);
                if (exe[0] && SUCCEEDED(IAudioSessionControl2_QueryInterface(sc2, &IID_ISimpleAudioVolume, (void **)&sv))) {
                    Row *r = NULL;
                    for (int k = first; k < g_n; k++) if (!_wcsicmp(g_rows[k].key, exe)) { r = &g_rows[k]; break; }
                    if (!r && g_n < MAX_ROWS) {
                        r = &g_rows[g_n++]; memset(r, 0, sizeof *r); r->kind = 1; r->dev = devIdx; wcscpy(r->key, exe);
                        if (sys) { wcscpy(r->name, L"Sons do sistema"); r->glyph = TRUE; r->glyphChar = 0xE767; }
                        else { if (friendly[0]) wcscpy(r->name, friendly); else PrettyName(exe, r->name, 96); r->icon = icon; icon = NULL; }
                        ISimpleAudioVolume_GetMasterVolume(sv, &r->vol); ISimpleAudioVolume_GetMute(sv, &r->mute);
                    }
                    if (r && r->nsv < MAX_SV) {
                        r->sv[r->nsv++] = sv; sv = NULL;
                        IAudioMeterInformation *mi = NULL;
                        if (SUCCEEDED(IAudioSessionControl_QueryInterface(sc, &IID_IAudioMeterInformation, (void **)&mi))) r->meter[r->nmeter++] = mi;
                    }
                }
                if (sv) ISimpleAudioVolume_Release(sv);
                if (icon) DestroyIcon(icon);
                IAudioSessionControl2_Release(sc2);
            }
            IAudioSessionControl_Release(sc);
        }
        IAudioSessionEnumerator_Release(se);
    }
    IAudioSessionManager2_Release(sm);
    if (g_n - first > 1) qsort(&g_rows[first], g_n - first, sizeof(Row), CmpRows);
}

static void Enumerate(void) {
    FreeRows(); g_devCount = 0;
    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en))) return;
    IMMDevice *def = NULL; LPWSTR defId = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &def))) { IMMDevice_GetId(def, &defId); AddDevice(def); IMMDevice_Release(def); }
    IMMDeviceCollection *col = NULL; UINT cnt = 0;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) {
        IMMDeviceCollection_GetCount(col, &cnt);
        for (UINT i = 0; i < cnt; i++) {
            IMMDevice *dev = NULL; LPWSTR id = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &dev))) continue;
            IMMDevice_GetId(dev, &id);
            if (!(defId && id && !wcscmp(id, defId))) AddDevice(dev);   // o padrao ja entrou primeiro
            CoTaskMemFree(id); IMMDevice_Release(dev);
        }
        IMMDeviceCollection_Release(col);
    }
    CoTaskMemFree(defId); IMMDeviceEnumerator_Release(en);
}

static void RefreshValues(void) {        // so atualiza numeros (mudancas feitas em outro lugar), sem re-enumerar
    for (int i = 0; i < g_n; i++) {
        Row *r = &g_rows[i];
        if (r->hidden || GetTickCount64() - g_lastSetTick < 500) continue;   // logo apos mexer, nao sobrescreve o valor
        if (r->kind == 0 && r->ev) { IAudioEndpointVolume_GetMasterVolumeLevelScalar(r->ev, &r->vol); IAudioEndpointVolume_GetMute(r->ev, &r->mute); }
        else if (r->nsv) { ISimpleAudioVolume_GetMasterVolume(r->sv[0], &r->vol); ISimpleAudioVolume_GetMute(r->sv[0], &r->mute); }
    }
}

static BOOL ReadPeaks(void) {            // le o nivel de cada linha; devolve TRUE se algo mudou
    BOOL changed = FALSE;
    for (int i = 0; i < g_n; i++) {
        Row *r = &g_rows[i]; if (r->hidden) continue;
        float best = 0;
        for (int j = 0; j < r->nmeter; j++) { float p = 0; if (SUCCEEDED(IAudioMeterInformation_GetPeakValue(r->meter[j], &p)) && p > best) best = p; }
        if (fabsf(best - r->peak) > 0.01f) { r->peak = best; changed = TRUE; }
    }
    return changed;
}

// O envio do volume ao Windows e sincrono e caro (ate ~20 ms por sessao). Para a interface nunca
// esperar, uma thread auxiliar faz o envio: a UI so anota o ultimo valor; se chegarem varios
// movimentos enquanto um envio esta em curso, so o ultimo e enviado.
static DWORD WINAPI ApplyThread(LPVOID p) {
    (void)p; CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        WaitForSingleObject(g_evt, INFINITE); if (g_quit) break;
        for (;;) {
            IAudioEndpointVolume *ev = NULL; ISimpleAudioVolume *sv[MAX_SV]; int nsv = 0; float vol;
            EnterCriticalSection(&g_cs);
            if (g_pendingRow < 0) { LeaveCriticalSection(&g_cs); break; }
            Row *r = &g_rows[g_pendingRow]; vol = g_pendingVol; g_pendingRow = -1;
            if (r->kind == 0) { ev = r->ev; if (ev) IAudioEndpointVolume_AddRef(ev); }
            else for (int j = 0; j < r->nsv; j++) { sv[nsv] = r->sv[j]; ISimpleAudioVolume_AddRef(sv[nsv]); nsv++; }
            LeaveCriticalSection(&g_cs);                  // objetos com AddRef: seguros mesmo se a lista for refeita
            if (ev) { IAudioEndpointVolume_SetMasterVolumeLevelScalar(ev, vol, NULL); IAudioEndpointVolume_Release(ev); }
            for (int j = 0; j < nsv; j++) { ISimpleAudioVolume_SetMasterVolume(sv[j], vol, NULL); ISimpleAudioVolume_Release(sv[j]); }
        }
    }
    CoUninitialize(); return 0;
}
static void FlushVol(void) { if (g_evt) SetEvent(g_evt); }
static void SetVol(Row *r, float v) {    // atualiza a tela ja; a thread auxiliar envia ao Windows
    if (v < 0) v = 0; if (v > 1) v = 1; r->vol = v;
    EnterCriticalSection(&g_cs); g_pendingRow = (int)(r - g_rows); g_pendingVol = v; LeaveCriticalSection(&g_cs);
    g_lastSetTick = GetTickCount64();
    SetEvent(g_evt);
}
static void SetMute(Row *r, BOOL m) {
    r->mute = m;
    if (r->kind == 0) { if (r->ev) IAudioEndpointVolume_SetMute(r->ev, m, NULL); }
    else for (int j = 0; j < r->nsv; j++) ISimpleAudioVolume_SetMute(r->sv[j], m, NULL);
}

// ---------------------------------------------------------------- layout e desenho
static void Layout(void) {
    int y = PAD / 2;
    for (int i = 0; i < g_n; i++) {
        Row *r = &g_rows[i];
        r->hidden = !g_showAll && r->dev != 0;
        if (r->hidden) { r->y = -100000; r->h = 0; continue; }
        if (r->kind == 0 && i > 0) y += GAP_DEV;
        r->y = y; r->h = r->kind == 0 ? ROW_DEV : ROW_APP; y += r->h;
    }
    g_contentH = y + PAD / 2;
    if (g_scroll > g_contentH - g_viewH) g_scroll = g_contentH - g_viewH;
    if (g_scroll < 0) g_scroll = 0;
}
static void SliderRect(const Row *r, RECT *rc) {           // trilho do slider em coordenadas da janela
    int cy = r->y - g_scroll + (r->kind == 0 ? r->h - S(18) : r->h - S(16));   // app: slider abaixo do nome
    rc->left = SLIDER_X; rc->right = g_W - PAD - VALUE_W; rc->top = cy - S(10); rc->bottom = cy + S(10);
}

static void Paint(HDC dc, const RECT *clip) {
    RECT full = { 0, 0, g_W, g_viewH };
    HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, g_W, g_viewH); HGDIOBJ ob = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32)); FillRect(mem, &full, bg); DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    HBRUSH devBg = CreateSolidBrush(RGB(44, 44, 44)), accent = CreateSolidBrush(g_accent), track = CreateSolidBrush(RGB(90, 90, 90)), thumbMute = CreateSolidBrush(RGB(150, 150, 150)), peakBr = CreateSolidBrush(RGB(240, 240, 240));
    HPEN np = (HPEN)GetStockObject(NULL_PEN);
    for (int i = 0; i < g_n; i++) {
        Row *r = &g_rows[i]; int top = r->y - g_scroll;
        if (r->hidden || top + r->h < 0 || top > g_viewH) continue;
        if (clip && (top + r->h <= clip->top || top >= clip->bottom)) continue;   // fora da area a redesenhar
        RECT rr = { 0, top, g_W, top + r->h };
        if (r->kind == 0) { FillRect(mem, &rr, devBg); }
        if (r->kind == 0 && r->dev == 0 && g_devCount > 1) {   // seta: recolher/expandir os outros dispositivos
            HPEN cp = CreatePen(PS_SOLID, S(2), RGB(200, 200, 200)); HGDIOBJ op = SelectObject(mem, cp);
            int cx = g_W - PAD - S(8), cy = top + S(18), a = S(5);
            if (g_showAll) { MoveToEx(mem, cx - a, cy + a / 2, NULL); LineTo(mem, cx, cy - a / 2); LineTo(mem, cx + a + 1, cy + a / 2 + 1); }
            else           { MoveToEx(mem, cx - a, cy - a / 2, NULL); LineTo(mem, cx, cy + a / 2); LineTo(mem, cx + a + 1, cy - a / 2 - 1); }
            SelectObject(mem, op); DeleteObject(cp);
        }
        // icone
        int iy = (r->kind == 0 ? top + r->h - S(18) : top + r->h - S(16)) - ICON_SZ / 2;   // alinhado ao slider
        if (r->glyph) {                                      // alto-falante vetorial (Segoe Fluent Icons); mudo = glifo proprio
            RECT gr = { PAD + S(2), iy, PAD + S(2) + ICON_SZ, iy + ICON_SZ };
            SelectObject(mem, g_fontIcon); SetTextColor(mem, r->mute ? RGB(230, 70, 70) : RGB(225, 225, 225));
            WCHAR gch[2] = { r->mute ? 0xE74F : r->glyphChar, 0 };
            DrawTextW(mem, gch, 1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        } else if (r->icon) DrawIconEx(mem, PAD + S(2), iy, r->icon, ICON_SZ, ICON_SZ, 0, NULL, DI_NORMAL);
        if (r->mute && !r->glyph) {                          // risco vermelho sobre o icone = mudo
            HPEN rp = CreatePen(PS_SOLID, S(2), RGB(230, 70, 70)); HGDIOBJ op = SelectObject(mem, rp);
            MoveToEx(mem, PAD + S(2), iy + ICON_SZ, NULL); LineTo(mem, PAD + S(2) + ICON_SZ, iy); SelectObject(mem, op); DeleteObject(rp);
        }
        // nome: dispositivo em negrito no topo; aplicativo em texto pequeno acima do slider
        if (r->kind == 0) {
            RECT tr = { PAD, top + S(8), g_W - PAD - S(28), top + S(28) };
            SelectObject(mem, g_fontBold); SetTextColor(mem, RGB(235, 235, 235));
            DrawTextW(mem, r->name, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            RECT tr = { SLIDER_X, top + S(3), g_W - PAD - VALUE_W, top + S(17) };
            SelectObject(mem, g_fontSmall); SetTextColor(mem, RGB(170, 170, 170));
            DrawTextW(mem, r->name, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        // slider
        RECT sr; SliderRect(r, &sr); int cy = (sr.top + sr.bottom) / 2, th = S(4);
        RECT t1 = { sr.left, cy - th / 2, sr.right, cy + th / 2 }; FillRect(mem, &t1, track);
        int x = sr.left + (int)((sr.right - sr.left) * r->vol + 0.5f);
        RECT t2 = { sr.left, cy - th / 2, x, cy + th / 2 }; FillRect(mem, &t2, r->mute ? thumbMute : accent);
        if (r->peak > 0.005f && !r->mute) {                   // medidor de nivel: linha clara sobre o trilho
            int px = sr.left + (int)((sr.right - sr.left) * r->peak + 0.5f);
            RECT pk = { sr.left, cy - S(1), px, cy + S(1) }; FillRect(mem, &pk, peakBr);
        }
        HGDIOBJ obr = SelectObject(mem, r->mute ? thumbMute : accent), opn = SelectObject(mem, np);
        Ellipse(mem, x - THUMB_R, cy - THUMB_R, x + THUMB_R + 1, cy + THUMB_R + 1);
        SelectObject(mem, obr); SelectObject(mem, opn);
        // valor
        WCHAR v[8]; _snwprintf(v, 8, L"%d", (int)(r->vol * 100 + 0.5f));
        RECT vr = { sr.right + S(6), cy - S(12), g_W - PAD, cy + S(12) };
        SelectObject(mem, r->kind == 0 ? g_fontBold : g_font); SetTextColor(mem, r->mute ? RGB(150, 150, 150) : RGB(235, 235, 235));
        DrawTextW(mem, v, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (g_n == 0) {
        SelectObject(mem, g_font); SetTextColor(mem, RGB(180, 180, 180));
        DrawTextW(mem, L"Nenhum dispositivo de som ativo", -1, &full, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    DeleteObject(devBg); DeleteObject(accent); DeleteObject(track); DeleteObject(thumbMute); DeleteObject(peakBr);
    if (clip) BitBlt(dc, clip->left, clip->top, clip->right - clip->left, clip->bottom - clip->top, mem, clip->left, clip->top, SRCCOPY);
    else      BitBlt(dc, 0, 0, g_W, g_viewH, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
}

// ---------------------------------------------------------------- interacao
static int RowAt(int y) { for (int i = 0; i < g_n; i++) { if (g_rows[i].hidden) continue; int top = g_rows[i].y - g_scroll; if (y >= top && y < top + g_rows[i].h) return i; } return -1; }
static void Resize(void);
static float VolFromX(const Row *r, int x) { RECT sr; SliderRect(r, &sr); return (float)(x - sr.left) / (float)(sr.right - sr.left); }

static LRESULT CALLBACK MixerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); Paint(dc, &ps.rcPaint); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: {
        if (g_animDir < 0) return 0;                          // fechando: ignora cliques
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l), i = RowAt(y); if (i < 0) return 0;
        Row *r = &g_rows[i]; RECT sr; SliderRect(r, &sr);
        if (r->kind == 0 && r->dev == 0 && g_devCount > 1 && x > g_W - PAD - S(28) && y < r->y - g_scroll + S(34)) {
            g_showAll = !g_showAll; Resize(); return 0;      // clique na seta
        }
        if (x < SLIDER_X - S(4)) { SetMute(r, !r->mute); InvalidateRect(h, NULL, FALSE); return 0; }
        if (x >= sr.left - THUMB_R && x <= sr.right + THUMB_R) {
            g_drag = i; SetCapture(h); SetVol(r, VolFromX(r, x)); if (r->mute) SetMute(r, FALSE);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drag >= 0 && (w & MK_LBUTTON)) {
            Row *r = &g_rows[g_drag]; float nv = VolFromX(r, GET_X_LPARAM(l)); if (nv < 0) nv = 0; if (nv > 1) nv = 1;
            if ((int)(nv * 100 + 0.5f) != (int)(r->vol * 100 + 0.5f)) {      // so quando o numero muda
                SetVol(r, nv);
                RECT rr = { 0, r->y - g_scroll, g_W, r->y - g_scroll + r->h }; InvalidateRect(h, &rr, FALSE);   // so a linha
            }
        }
        return 0;
    case WM_LBUTTONUP: if (g_drag >= 0) { g_drag = -1; ReleaseCapture(); FlushVol(); PlayVolumeSound(); } return 0;
    case WM_CAPTURECHANGED: g_drag = -1; FlushVol(); return 0;
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        int i = RowAt(p.y); int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (i >= 0 && p.x >= SLIDER_X - S(4)) { Row *r = &g_rows[i]; SetVol(r, r->vol + (delta > 0 ? 0.02f : -0.02f)); if (r->mute && delta > 0) SetMute(r, FALSE); SetTimer(h, TIMER_BEEP, 250, NULL); }
        else { g_scroll -= delta / WHEEL_DELTA * S(40); Layout(); }
        InvalidateRect(h, NULL, FALSE); return 0;
    }
    case WM_KEYDOWN: if (w == VK_ESCAPE) Mixer_Close(); return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) Mixer_Close(); return 0;
    case WM_TIMER:
        if (w == TIMER_BEEP) { KillTimer(h, TIMER_BEEP); PlayVolumeSound(); return 0; }
        if (w == TIMER_PEAK) { if (g_animDir >= 0 && ReadPeaks()) InvalidateRect(h, NULL, FALSE); return 0; }
        if (w == TIMER_ANIM) {                                // sobe de baixo com fade (abrir) / fade curto (fechar)
            ULONGLONG el = GetTickCount64() - g_animStart; int dur = g_animDir > 0 ? ANIM_OPEN_MS : ANIM_CLOSE_MS;
            float t = el >= (ULONGLONG)dur ? 1.0f : (float)el / dur;
            float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   // ease-out cubico
            if (g_animDir > 0) {
                SetWindowPos(h, NULL, g_x, g_finalY + (int)((1.0f - e) * S(30)), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                SetLayeredWindowAttributes(h, 0, (BYTE)(e * 255), LWA_ALPHA);
                if (t >= 1.0f) { KillTimer(h, TIMER_ANIM); g_animDir = 0; }
            } else {
                SetLayeredWindowAttributes(h, 0, (BYTE)((1.0f - t) * 255), LWA_ALPHA);
                SetWindowPos(h, NULL, g_x, g_finalY + (int)(t * S(12)), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (t >= 1.0f) { KillTimer(h, TIMER_ANIM); g_animDir = 0; DestroyWindow(h); }
            }
            return 0;
        }
        if (g_drag < 0) { RefreshValues(); InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_DESTROY: FlushVol(); KillTimer(h, TIMER_REFRESH); KillTimer(h, TIMER_ANIM); KillTimer(h, TIMER_PEAK); KillTimer(h, TIMER_BEEP); FreeRows(); g_wnd = NULL; g_animDir = 0; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- API
void Mixer_Init(HINSTANCE hinst, HFONT font, int dpi) {
    g_hinst = hinst; g_dpi = dpi; g_font = font;
    InitializeCriticalSection(&g_cs);
    g_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, ApplyThread, NULL, 0, NULL);
    g_fontBold = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontSmall = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    WNDCLASSW wc = {0}; wc.lpfnWndProc = MixerProc; wc.hInstance = hinst; wc.lpszClassName = L"PainelLeveMixer";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); RegisterClassW(&wc);
    DWORD col = 0; BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&col, &opaque))) g_accent = RGB((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
    // fonte de icones do Windows 11 (cai para a do Windows 10 se nao existir)
    g_fontIcon = CreateFontW(-S(20), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe Fluent Icons");
    LOGFONTW lf; GetObjectW(g_fontIcon, sizeof lf, &lf);
    HDC dc = GetDC(NULL); HGDIOBJ of = SelectObject(dc, g_fontIcon); WCHAR face[LF_FACESIZE]; GetTextFaceW(dc, LF_FACESIZE, face); SelectObject(dc, of); ReleaseDC(NULL, dc);
    if (_wcsicmp(face, L"Segoe Fluent Icons")) { DeleteObject(g_fontIcon); g_fontIcon = CreateFontW(-S(20), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets"); }
}

BOOL Mixer_IsOpen(void) { return g_wnd != NULL; }
static void StartAnim(int dir) { g_animDir = dir; g_animStart = GetTickCount64(); SetTimer(g_wnd, TIMER_ANIM, 10, NULL); }
void Mixer_Close(void) {
    if (!g_wnd || g_animDir < 0) return;
    g_lastClose = GetTickCount64();
    KillTimer(g_wnd, TIMER_REFRESH); KillTimer(g_wnd, TIMER_PEAK); ReleaseCapture(); g_drag = -1; FlushVol();
    StartAnim(-1);                                            // DestroyWindow ao fim do fade
}

static void Resize(void) {               // recalcula altura e mantem o painel encostado na barra
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_scroll = 0; g_viewH = 1; Layout();
    int maxH = (wa.bottom - wa.top) * 3 / 4;
    g_viewH = g_contentH < maxH ? g_contentH : maxH; Layout();
    g_finalY = wa.bottom - g_viewH - S(12);
    if (g_wnd) { SetWindowPos(g_wnd, NULL, g_x, g_finalY, g_W, g_viewH, SWP_NOZORDER | SWP_NOACTIVATE); InvalidateRect(g_wnd, NULL, FALSE); }
}

void Mixer_Toggle(void) {
    if (g_wnd) { Mixer_Close(); return; }
    // o clique no icone da bandeja tira o foco do painel (que fecha) e so depois chega aqui:
    // se ele acabou de fechar, este clique era para fechar, nao para reabrir
    if (GetTickCount64() - g_lastClose < 400) return;
    Enumerate();
    g_W = S(360);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT cur; GetCursorPos(&cur);
    g_x = cur.x - g_W / 2;
    if (g_x + g_W > wa.right - S(12)) g_x = wa.right - g_W - S(12); if (g_x < wa.left + S(12)) g_x = wa.left + S(12);
    Resize();
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, L"PainelLeveMixer", L"Mixer de volume", WS_POPUP,
                            g_x, g_finalY + S(30), g_W, g_viewH, NULL, NULL, g_hinst, NULL);
    int corner = 2; DwmSetWindowAttribute(g_wnd, 33, &corner, sizeof corner);
    SetLayeredWindowAttributes(g_wnd, 0, 0, LWA_ALPHA);       // comeca invisivel, 30 px abaixo; a animacao traz para cima
    ShowWindow(g_wnd, SW_SHOW); SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    StartAnim(+1);
    SetTimer(g_wnd, TIMER_REFRESH, 1000, NULL);
    SetTimer(g_wnd, TIMER_PEAK, 50, NULL);
}
