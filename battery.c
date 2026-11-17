// Bateria do mouse Logitech sem fio (receptores Lightspeed / Unifying / Bolt) via protocolo HID++ 2.0.
// Abordagem baseada no projeto github.com/renanleonel/windows-taskbar-battery (MIT).
//
// Mensagem HID++: [report_id, indice_dispositivo, indice_feature, (funcao<<4)|sw_id, params...]
// No Windows cada tamanho de relatorio e uma colecao HID separada: escrevemos o relatorio longo
// (0x11, 20 bytes) na colecao "longa" e escutamos tambem a "curta" (0x10, 7 bytes), onde o receptor
// responde alguns erros (ex.: "nenhum dispositivo neste slot").
#define COBJMACROS
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <wchar.h>
#include "battery.h"

#define LOGI_VID 0x046D
#define REP_SHORT 0x10
#define REP_LONG 0x11
#define SW_ID 0x0B                 // nosso software id (distingue das respostas do G HUB)
#define TIMEOUT_MS 600
#define ERR_UNKNOWN_DEVICE 0x08

#define F_DEVICE_FW   0x0003
#define F_DEVICE_NAME 0x0005
#define F_BATT_STATUS 0x1000
#define F_BATT_VOLT   0x1001
#define F_BATT_UNIFIED 0x1004

static const USHORT RECEIVER_PIDS[] = {
    0xC517, 0xC518, 0xC51A, 0xC51B, 0xC521, 0xC525, 0xC526, 0xC52B, 0xC52E, 0xC52F, 0xC531, 0xC532,
    0xC534, 0xC537, 0xC539, 0xC53A, 0xC53D, 0xC53F, 0xC541, 0xC545, 0xC547, 0xC548, 0xC54D,
};

typedef struct { HANDLE h; OVERLAPPED ov; BYTE buf[64]; DWORD len; BOOL pending; } Chan;
typedef struct { Chan lng, sht; BOOL hasShort; } Link;

enum { R_OK, R_TIMEOUT, R_PROTO, R_IO };

// ---------- E/S HID ----------
static BOOL ChanOpen(Chan *c, const WCHAR *path) {
    ZeroMemory(c, sizeof *c);
    c->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (c->h == INVALID_HANDLE_VALUE) { c->h = NULL; return FALSE; }
    c->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    PHIDP_PREPARSED_DATA pp; HIDP_CAPS caps;
    c->len = 20;
    if (HidD_GetPreparsedData(c->h, &pp)) {
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength)
            c->len = caps.InputReportByteLength;
        HidD_FreePreparsedData(pp);
    }
    if (c->len > sizeof c->buf) c->len = sizeof c->buf;
    return TRUE;
}

static void ChanClose(Chan *c) {
    if (!c->h) return;
    if (c->pending) { DWORD n; CancelIoEx(c->h, &c->ov); GetOverlappedResult(c->h, &c->ov, &n, TRUE); }
    CloseHandle(c->ov.hEvent); CloseHandle(c->h); c->h = NULL; c->pending = FALSE;
}

// garante uma leitura pendente; devolve TRUE se ha dados ja prontos em buf (n bytes)
static BOOL ChanArm(Chan *c, DWORD *n) {
    *n = 0;
    if (c->pending) return FALSE;
    ResetEvent(c->ov.hEvent);
    if (ReadFile(c->h, c->buf, c->len, n, &c->ov)) return TRUE;
    if (GetLastError() == ERROR_IO_PENDING) { c->pending = TRUE; return FALSE; }
    return FALSE;
}

static BOOL ChanDone(Chan *c, DWORD *n) {
    c->pending = FALSE;
    return GetOverlappedResult(c->h, &c->ov, n, FALSE);
}

// confere se o relatorio e a resposta da nossa requisicao
static int Match(const BYTE *rx, DWORD n, BYTE idx, BYTE feat, BYTE fsw, BYTE out[16], BYTE *err) {
    if (n < 6 || (rx[0] != REP_SHORT && rx[0] != REP_LONG) || rx[1] != idx) return -1;
    if ((rx[2] == 0xFF || rx[2] == 0x8F) && rx[3] == feat && rx[4] == fsw) { *err = rx[5]; return R_PROTO; }
    if (rx[2] == feat && rx[3] == fsw) {
        ZeroMemory(out, 16);
        DWORD m = n - 4; if (m > 16) m = 16;
        memcpy(out, rx + 4, m);
        return R_OK;
    }
    return -1;
}

static int Request(Link *l, BYTE idx, BYTE feat, BYTE func, const BYTE *params, int np, BYTE out[16], BYTE *err) {
    BYTE fsw = (BYTE)((func << 4) | SW_ID);
    BYTE tx[20] = { REP_LONG, idx, feat, fsw };
    if (np > 16) np = 16;
    if (np) memcpy(tx + 4, params, np);

    // descarta respostas antigas ja recebidas e arma leituras antes de escrever
    DWORD n;
    Chan *ch[2] = { &l->lng, l->hasShort ? &l->sht : NULL };
    for (int i = 0; i < 2; i++) if (ch[i]) {
        while (ChanArm(ch[i], &n)) { int r = Match(ch[i]->buf, n, idx, feat, fsw, out, err); if (r >= 0) return r; }
    }

    OVERLAPPED wo = {0}; wo.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD wr = 0;
    BOOL ok = WriteFile(l->lng.h, tx, sizeof tx, &wr, &wo);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(wo.hEvent, 1000) != WAIT_OBJECT_0) CancelIoEx(l->lng.h, &wo);
        ok = GetOverlappedResult(l->lng.h, &wo, &wr, TRUE);
    }
    CloseHandle(wo.hEvent);
    if (!ok) return R_IO;

    DWORD deadline = GetTickCount() + TIMEOUT_MS;
    for (;;) {
        HANDLE ev[2]; Chan *who[2]; int ne = 0;
        for (int i = 0; i < 2; i++) if (ch[i]) {
            while (ChanArm(ch[i], &n)) { int r = Match(ch[i]->buf, n, idx, feat, fsw, out, err); if (r >= 0) return r; }
            if (ch[i]->pending) { ev[ne] = ch[i]->ov.hEvent; who[ne++] = ch[i]; }
        }
        if (!ne) return R_IO;
        DWORD now = GetTickCount();
        if ((int)(deadline - now) <= 0) return R_TIMEOUT;
        DWORD w = WaitForMultipleObjects(ne, ev, FALSE, deadline - now);
        if (w == WAIT_TIMEOUT) return R_TIMEOUT;
        if (w >= WAIT_OBJECT_0 + (DWORD)ne) return R_IO;
        Chan *c = who[w - WAIT_OBJECT_0];
        if (ChanDone(c, &n)) { int r = Match(c->buf, n, idx, feat, fsw, out, err); if (r >= 0) return r; }
    }
}

static int FeatureIndex(Link *l, BYTE idx, USHORT feature, BYTE *fi, BYTE *err) {
    BYTE p[2] = { (BYTE)(feature >> 8), (BYTE)feature }, r[16];
    int rc = Request(l, idx, 0x00, 0x0, p, 2, r, err);
    if (rc == R_OK) *fi = r[0];
    return rc;
}

// ---------- curva de descarga (tensao -> %) ----------
// Com o G HUB instalado usamos a curva dele (C:\ProgramData\LGHUB\depots\...\*battery*.xml) para o %
// bater com o do G HUB; senao, curva generica de Li-ion (aproximacao do Solaar).
typedef struct { USHORT pid; int n; USHORT *mv; float *used; float total; } Curve;
static Curve g_curves[32];
static int g_ncurves;

static void ParseCurve(const char *t) {
    const char *pm = strstr(t, "<powermodel"); if (!pm) return;
    const char *pp = strstr(pm, "pid=\"0x"); if (!pp) return;
    USHORT pid = (USHORT)strtoul(pp + 7, NULL, 16);
    const char *d = strstr(t, "<discharge"); if (!d) return;
    d = strstr(d, "<![CDATA["); if (!d) return; d += 9;
    const char *end = strstr(d, "]]>"); if (!end) return;
    int cap = 0; for (const char *q = d; q < end; q++) if (*q == '\n') cap++;
    if (cap < 2 || g_ncurves >= 32) return;
    Curve *c = &g_curves[g_ncurves];
    c->mv = malloc(sizeof(USHORT) * (cap + 1)); c->used = malloc(sizeof(float) * (cap + 1)); c->n = 0;
    const char *line = d;
    while (line < end) {
        const char *nl = memchr(line, '\n', end - line); if (!nl) nl = end;
        char buf[128]; size_t L = nl - line; if (L >= sizeof buf) L = sizeof buf - 1;
        memcpy(buf, line, L); buf[L] = 0;
        float tm, v, u;
        if (sscanf(buf, " %f , %f , %f", &tm, &v, &u) == 3 && c->n <= cap) {
            c->mv[c->n] = (USHORT)(v * 1000.0f + 0.5f); c->used[c->n] = u; c->n++;
        }
        line = nl + 1;
    }
    if (c->n < 2 || c->used[c->n - 1] <= 0) { free(c->mv); free(c->used); return; }
    c->total = c->used[c->n - 1]; c->pid = pid; g_ncurves++;
}

static void CollectCurves(const WCHAR *dir, int depth) {
    WCHAR pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%s\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        WCHAR p[MAX_PATH]; swprintf(p, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 3) CollectCurves(p, depth + 1); continue; }
        size_t L = wcslen(fd.cFileName);
        if (L < 4 || _wcsicmp(fd.cFileName + L - 4, L".xml") || !wcsstr(fd.cFileName, L"battery")) continue;
        HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD sz = GetFileSize(h, NULL), rd = 0;
        if (sz && sz < 4 * 1024 * 1024) {
            char *t = malloc(sz + 1);
            if (ReadFile(h, t, sz, &rd, NULL)) { t[rd] = 0; ParseCurve(t); }
            free(t);
        }
        CloseHandle(h);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static int GenericPercent(int mv) {
    static const int C[][2] = { {4186,100},{4067,90},{3989,80},{3922,70},{3859,60},{3811,50},{3778,40},
                                {3751,30},{3717,20},{3671,10},{3646,5},{3579,2},{3500,0} };
    if (mv >= C[0][0]) return 100;
    for (int i = 0; i < 12; i++)
        if (mv >= C[i + 1][0]) {
            float t = (float)(mv - C[i + 1][0]) / (C[i][0] - C[i + 1][0]);
            return (int)(C[i + 1][1] + t * (C[i][1] - C[i + 1][1]) + 0.5f);
        }
    return 0;
}

static int VoltPercent(const USHORT *pids, int np, int mv) {
    for (int i = 0; i < np; i++)
        for (int k = 0; k < g_ncurves; k++)
            if (g_curves[k].pid == pids[i]) {
                Curve *c = &g_curves[k];
                if (mv >= c->mv[0]) return 100;
                float used = c->total;
                for (int j = 0; j < c->n; j++) if (c->mv[j] <= mv) { used = c->used[j]; break; }
                int p = (int)(100.0f * (1.0f - used / c->total) + 0.5f);
                return p < 0 ? 0 : p > 100 ? 100 : p;
            }
    return GenericPercent(mv);
}

// ---------- descoberta e leitura ----------
typedef struct { WCHAR name[64]; int kind; USHORT pids[4]; int npids; BOOL known; } Ident;
static Ident g_ident[7];          // por indice 1..6 do receptor (um receptor basta)

static void Identify(Link *l, BYTE idx, Ident *id) {
    BYTE fi, r[16], err;
    id->kind = -1; id->name[0] = 0; id->npids = 0;
    if (FeatureIndex(l, idx, F_DEVICE_NAME, &fi, &err) == R_OK && fi) {
        if (Request(l, idx, fi, 0, NULL, 0, r, &err) == R_OK) {
            int len = r[0]; char s[64] = {0}; int got = 0;
            if (len > 63) len = 63;
            while (got < len) {
                BYTE off = (BYTE)got;
                if (Request(l, idx, fi, 1, &off, 1, r, &err) != R_OK) break;
                int take = len - got; if (take > 16) take = 16;
                memcpy(s + got, r, take); got += take;
            }
            MultiByteToWideChar(CP_UTF8, 0, s, -1, id->name, 64);
        }
        if (Request(l, idx, fi, 2, NULL, 0, r, &err) == R_OK) id->kind = r[0];
    }
    if (FeatureIndex(l, idx, F_DEVICE_FW, &fi, &err) == R_OK && fi && Request(l, idx, fi, 0, NULL, 0, r, &err) == R_OK)
        for (int i = 0; i < 3; i++) {
            USHORT p = (USHORT)(r[7 + 2 * i] << 8 | r[8 + 2 * i]);
            if (p) id->pids[id->npids++] = p;
        }
    if (!id->name[0]) wcscpy(id->name, L"Mouse Logitech");
    id->known = TRUE;
}

// le a bateria do dispositivo idx: R_OK com *pct/*chg, ou codigo de erro; *nobatt se nao tem feature
static int ReadBattery(Link *l, BYTE idx, const Ident *id, int *pct, BOOL *chg, BYTE *err) {
    BYTE fi, r[16]; int rc;
    if ((rc = FeatureIndex(l, idx, F_BATT_UNIFIED, &fi, err)) != R_OK) return rc;
    if (fi) {
        if ((rc = Request(l, idx, fi, 1, NULL, 0, r, err)) != R_OK) return rc;
        *pct = r[0]; *chg = (r[2] == 1 || r[2] == 2); return R_OK;
    }
    if ((rc = FeatureIndex(l, idx, F_BATT_STATUS, &fi, err)) != R_OK) return rc;
    if (fi) {
        if ((rc = Request(l, idx, fi, 0, NULL, 0, r, err)) != R_OK) return rc;
        *pct = r[0]; *chg = (r[2] == 1 || r[2] == 2 || r[2] == 4); return R_OK;
    }
    if ((rc = FeatureIndex(l, idx, F_BATT_VOLT, &fi, err)) != R_OK) return rc;
    if (fi) {
        if ((rc = Request(l, idx, fi, 0, NULL, 0, r, err)) != R_OK) return rc;
        int mv = r[0] << 8 | r[1];
        *chg = (r[2] & 0x80) != 0;
        *pct = VoltPercent(id->pids, id->npids, mv);
        return R_OK;
    }
    return R_PROTO; // sem feature de bateria
}

// encontra as colecoes HID++ (curta/longa) do primeiro receptor Logitech presente
static BOOL FindReceiver(WCHAR *shortPath, WCHAR *longPath) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return FALSE;
    shortPath[0] = longPath[0] = 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        BYTE detBuf[1024]; PSP_DEVICE_INTERFACE_DETAIL_DATA_W det = (void *)detBuf;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof detBuf, NULL, NULL)) continue;
        // abre so para consultar atributos (sem acesso de leitura/escrita: funciona ate em dispositivos exclusivos)
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = { sizeof at };
        BOOL rcv = FALSE;
        if (HidD_GetAttributes(h, &at) && at.VendorID == LOGI_VID)
            for (size_t k = 0; k < sizeof RECEIVER_PIDS / sizeof *RECEIVER_PIDS; k++) if (RECEIVER_PIDS[k] == at.ProductID) rcv = TRUE;
        if (rcv) {
            PHIDP_PREPARSED_DATA pp; HIDP_CAPS caps;
            if (HidD_GetPreparsedData(h, &pp)) {
                if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage >= 0xFF00) {
                    if ((caps.Usage & 0xFF) == 0x01 && !shortPath[0]) wcscpy(shortPath, det->DevicePath);
                    if ((caps.Usage & 0xFF) == 0x02 && !longPath[0]) wcscpy(longPath, det->DevicePath);
                }
                HidD_FreePreparsedData(pp);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return longPath[0] != 0;
}

// ---------- estado compartilhado + thread ----------
static CRITICAL_SECTION g_cs;
static BatteryInfo g_info;
static HWND g_notify; static UINT g_msg;
static HANDLE g_thread, g_stop, g_wake;
static BOOL g_curvesLoaded;

static void Publish(const BatteryInfo *b) {
    EnterCriticalSection(&g_cs);
    BOOL changed = memcmp(&g_info, b, sizeof *b) != 0;
    g_info = *b;
    LeaveCriticalSection(&g_cs);
    if (changed && g_notify) PostMessageW(g_notify, g_msg, 0, 0);
}

// uma varredura; devolve TRUE se leu algum mouse
static BOOL Scan(BatteryInfo *out) {
    if (!g_curvesLoaded) { CollectCurves(L"C:\\ProgramData\\LGHUB\\depots", 0); g_curvesLoaded = TRUE; }
    WCHAR sp[512], lp[512];
    if (!FindReceiver(sp, lp)) { ZeroMemory(g_ident, sizeof g_ident); return FALSE; }
    Link l = {0};
    if (!ChanOpen(&l.lng, lp)) return FALSE;
    l.hasShort = sp[0] && ChanOpen(&l.sht, sp);
    BOOL found = FALSE;
    for (BYTE idx = 1; idx <= 6 && !found; idx++) {
        Ident *id = &g_ident[idx];
        int pct = 0; BOOL chg = FALSE; BYTE err = 0;
        Ident tmp = {0};
        int rc = ReadBattery(&l, idx, id->known ? id : &tmp, &pct, &chg, &err);
        // dispositivo dormindo: a primeira requisicao o acorda; so insiste se ainda nao o conhecemos
        for (int tries = 0; rc == R_TIMEOUT && !id->known && tries < 2; tries++) {
            if (WaitForSingleObject(g_stop, 500) == WAIT_OBJECT_0) break;
            rc = ReadBattery(&l, idx, &tmp, &pct, &chg, &err);
        }
        if (rc == R_PROTO && err == ERR_UNKNOWN_DEVICE) { id->known = FALSE; continue; }   // slot vazio
        if (rc == R_OK) {
            if (!id->known) {
                Identify(&l, idx, id);
                ReadBattery(&l, idx, id, &pct, &chg, &err);   // rele com os pids do modelo (curva do G HUB)
            }
            // so mouses (tipos 3..5); teclados etc. ficam de fora
            if (id->kind != -1 && (id->kind < 3 || id->kind > 5)) continue;
            out->present = TRUE; out->stale = FALSE; out->percent = pct; out->charging = chg;
            wcscpy(out->name, id->name);
            found = TRUE;
        } else if (rc == R_TIMEOUT && id->known && (id->kind == -1 || (id->kind >= 3 && id->kind <= 5))) {
            out->present = TRUE; out->stale = TRUE;           // mantem o ultimo valor, esmaecido
            wcscpy(out->name, id->name);
            found = TRUE;
        }
    }
    ChanClose(&l.sht); ChanClose(&l.lng);
    return found;
}

static DWORD WINAPI Worker(LPVOID p) {
    (void)p;
    for (;;) {
        BatteryInfo b;
        EnterCriticalSection(&g_cs); b = g_info; LeaveCriticalSection(&g_cs);
        BatteryInfo n = b;
        if (!Scan(&n)) n.present = FALSE;
        if (!n.present) ZeroMemory(&n, sizeof n);
        Publish(&n);
        HANDLE hs[2] = { g_stop, g_wake };
        if (WaitForMultipleObjects(2, hs, FALSE, 15000) == WAIT_OBJECT_0) break;
    }
    return 0;
}

void Battery_Start(HWND notify, UINT msg) {
    if (g_thread) return;
    InitializeCriticalSection(&g_cs);
    g_notify = notify; g_msg = msg;
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
}

void Battery_Refresh(void) { if (g_wake) SetEvent(g_wake); }

void Battery_Stop(void) {
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread); g_thread = NULL;
}

BOOL Battery_Get(BatteryInfo *out) {
    if (!g_thread) { ZeroMemory(out, sizeof *out); return FALSE; }
    EnterCriticalSection(&g_cs); *out = g_info; LeaveCriticalSection(&g_cs);
    return out->present;
}

#ifdef BATTERY_TEST
int main(void) {
    InitializeCriticalSection(&g_cs);
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    CollectCurves(L"C:\\ProgramData\\LGHUB\\depots", 0); g_curvesLoaded = TRUE;
    printf("curvas G HUB: %d\n", g_ncurves);
    for (int i = 0; i < g_ncurves; i++) printf("  pid %04X, %d pontos\n", g_curves[i].pid, g_curves[i].n);
    WCHAR sp[512], lp[512];
    if (!FindReceiver(sp, lp)) { printf("receptor nao encontrado\n"); return 1; }
    printf("curta: %ls\nlonga: %ls\n", sp, lp);
    for (int k = 0; k < 2; k++) {
        BatteryInfo b = {0};
        DWORD t = GetTickCount();
        BOOL ok = Scan(&b);
        printf("scan %d: ok=%d %ls %d%% carregando=%d stale=%d (%lu ms)\n", k, ok, b.name, b.percent, b.charging, b.stale, GetTickCount() - t);
        for (int i = 1; i <= 6; i++) if (g_ident[i].known) {
            printf("  slot %d: %ls tipo=%d pids:", i, g_ident[i].name, g_ident[i].kind);
            for (int j = 0; j < g_ident[i].npids; j++) printf(" %04X", g_ident[i].pids[j]);
            printf("\n");
        }
    }
    return 0;
}
#endif
