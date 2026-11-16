// PainelLeveTap.dll - carregada dentro do Explorer pelo servico de diagnostico XAML
// (InitializeXamlDiagnosticsEx). Oculta o fundo em XAML da barra de tarefas do Windows 11
// (elementos "BackgroundFill" e "BackgroundStroke"), deixando so o efeito do DWM, que o
// PainelLeve.exe controla. O exe fala com esta DLL por uma janela message-only
// "PainelLeveTapWnd": WM_APP+1 com wParam 1 = transparente, 0 = normal.

#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cwchar>

// {7A1B2C3D-4E5F-4A6B-9C8D-0E1F2A3B4C5D}
static const CLSID CLSID_PainelLeveTap = { 0x7a1b2c3d, 0x4e5f, 0x4a6b, { 0x9c, 0x8d, 0x0e, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d } };
#define WM_TAP_SET (WM_APP + 1)

static HMODULE g_mod;
static LONG g_objs;
static int g_logLines;

static void Log(const wchar_t *fmt, ...) {
    if (g_logLines++ > 600) return;                       // nunca deixa o log crescer sem limite
    wchar_t path[MAX_PATH]; GetModuleFileNameW(g_mod, path, MAX_PATH);
    wchar_t *s = wcsrchr(path, L'\\'); if (!s) return; wcscpy(s + 1, L"tap.log");
    FILE *f = _wfopen(path, L"a, ccs=UTF-8"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(f, L"[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfwprintf(f, fmt, ap); va_end(ap);
    fputwc(L'\n', f); fclose(f);
}

static bool IsBackground(BSTR name) {
    return name && (!wcscmp(name, L"BackgroundFill") || !wcscmp(name, L"BackgroundStroke"));
}

struct Tap : IObjectWithSite, IVisualTreeServiceCallback2 {
    LONG ref = 1;
    IXamlDiagnostics *diag = nullptr;
    IVisualTreeService3 *vts = nullptr;
    std::vector<InstanceHandle> elems;
    bool on = false;
    HWND wnd = nullptr;

    Tap() { InterlockedIncrement(&g_objs); }
    ~Tap() { InterlockedDecrement(&g_objs); }

    // ---- IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IObjectWithSite) *ppv = static_cast<IObjectWithSite *>(this);
        else if (riid == IID_IVisualTreeServiceCallback || riid == IID_IVisualTreeServiceCallback2) *ppv = static_cast<IVisualTreeServiceCallback2 *>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }

    // ---- IObjectWithSite
    STDMETHODIMP SetSite(IUnknown *site) override {
        if (site) {
            HRESULT hr = site->QueryInterface(IID_IXamlDiagnostics, (void **)&diag);
            if (SUCCEEDED(hr)) hr = diag->QueryInterface(IID_IVisualTreeService3, (void **)&vts);
            if (SUCCEEDED(hr)) hr = vts->AdviseVisualTreeChange(this);
            Log(L"SetSite: hr=0x%08lX (thread %lu)", hr, GetCurrentThreadId());
            return hr;
        }
        Log(L"SetSite(null): limpando");
        if (vts) { vts->UnadviseVisualTreeChange(this); vts->Release(); vts = nullptr; }
        if (diag) { diag->Release(); diag = nullptr; }
        if (wnd) { DestroyWindow(wnd); wnd = nullptr; }
        return S_OK;
    }
    STDMETHODIMP GetSite(REFIID riid, void **ppv) override { return diag ? diag->QueryInterface(riid, ppv) : E_FAIL; }

    // ---- IVisualTreeServiceCallback(2)
    STDMETHODIMP OnVisualTreeChange(ParentChildRelation, VisualElement el, VisualMutationType mut) override {
        if (!wnd) CreateControlWindow();                   // criada na thread de UI do XAML
        if (mut == Add) {
            if (IsBackground(el.Name)) {
                Log(L"+ %s (%s) handle=%llu", el.Name, el.Type ? el.Type : L"?", (unsigned long long)el.Handle);
                elems.push_back(el.Handle);
                if (on) Apply(el.Handle, true);
            }
        } else {
            for (size_t i = 0; i < elems.size(); i++) if (elems[i] == el.Handle) { elems.erase(elems.begin() + i); break; }
        }
        return S_OK;
    }
    STDMETHODIMP OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override { return S_OK; }

    // ---- trabalho
    // GetPropertyIndex devolve E_INVALIDARG neste Windows; o indice vem da cadeia de propriedades
    // (GetPropertyValuesChain), que tambem informa o tipo exato do valor para o CreateInstance.
    struct Strategy { const wchar_t *prop, *value, *fallbackType; };
    static constexpr Strategy kStrategies[] = {
        { L"Visibility", L"Collapsed", L"Windows.UI.Xaml.Visibility" },
        { L"Opacity",    L"0",         L"Windows.Foundation.Double" },
        { L"Fill",       L"#00000000", L"Windows.UI.Xaml.Media.SolidColorBrush" },
    };
    struct PropInfo { bool found = false; unsigned idx = 0; wchar_t type[160] = L""; };
    int strategy = -1;
    unsigned strategyIdx = 0;                              // indice da propriedade escolhida (igual para todos os retangulos)

    PropInfo FindProp(InstanceHandle h, const wchar_t *name, bool verbose) {
        PropInfo r; unsigned sc = 0, pc = 0; PropertyChainSource *src = nullptr; PropertyChainValue *vals = nullptr;
        HRESULT hr = vts->GetPropertyValuesChain(h, &sc, &src, &pc, &vals);
        if (verbose) Log(L"GetPropertyValuesChain(%s): hr=0x%08lX props=%u", name, hr, pc);
        if (FAILED(hr)) return r;
        for (unsigned i = 0; i < pc; i++) {
            if (!r.found && vals[i].PropertyName && !wcscmp(vals[i].PropertyName, name)) {
                r.found = true; r.idx = vals[i].Index;
                if (vals[i].ValueType) wcsncpy(r.type, vals[i].ValueType, 159);
                if (verbose) Log(L"  %s: idx=%u tipo=%s valor=%s decl=%s bits=%lld", name, r.idx, r.type, vals[i].Value ? vals[i].Value : L"", vals[i].DeclaringType ? vals[i].DeclaringType : L"", (long long)vals[i].MetadataBits);
            }
            SysFreeString(vals[i].Type); SysFreeString(vals[i].DeclaringType); SysFreeString(vals[i].ValueType); SysFreeString(vals[i].ItemType); SysFreeString(vals[i].Value); SysFreeString(vals[i].PropertyName);
        }
        for (unsigned i = 0; i < sc; i++) { SysFreeString(src[i].TargetType); SysFreeString(src[i].Name); SysFreeString(src[i].SrcInfo.FileName); SysFreeString(src[i].SrcInfo.Hash); }
        CoTaskMemFree(vals); CoTaskMemFree(src);
        return r;
    }
    HRESULT CreateAndSet(InstanceHandle h, unsigned idx, const wchar_t *type, const wchar_t *value, bool verbose) {
        InstanceHandle v = 0;
        BSTR t = SysAllocString(type), val = SysAllocString(value);
        HRESULT hr = vts->CreateInstance(t, val, &v);
        SysFreeString(t); SysFreeString(val);
        if (verbose) Log(L"    CreateInstance(%s, %s): 0x%08lX handle=%llu", type, value, hr, (unsigned long long)v);
        if (FAILED(hr)) return hr;
        hr = vts->SetProperty(h, v, idx);
        if (verbose) Log(L"    SetProperty(idx %u): 0x%08lX", idx, hr);
        return hr;
    }
    void Apply(InstanceHandle h, bool hide) {
        const int n = (int)(sizeof kStrategies / sizeof kStrategies[0]);
        if (hide) {
            if (strategy >= 0) {
                HRESULT hr = CreateAndSet(h, strategyIdx, kStrategies[strategy].fallbackType, kStrategies[strategy].value, false);
                if (FAILED(hr)) Log(L"Apply(%llu) falhou: 0x%08lX", (unsigned long long)h, hr);
                return;
            }
            for (int i = 0; i < n; i++) {
                PropInfo p = FindProp(h, kStrategies[i].prop, true);
                if (!p.found) continue;
                if (p.type[0] && SUCCEEDED(CreateAndSet(h, p.idx, p.type, kStrategies[i].value, true))) { strategy = i; strategyIdx = p.idx; Log(L"estrategia: %s via tipo da cadeia", kStrategies[i].prop); return; }
                if (SUCCEEDED(CreateAndSet(h, p.idx, kStrategies[i].fallbackType, kStrategies[i].value, true))) { strategy = i; strategyIdx = p.idx; Log(L"estrategia: %s via tipo padrao", kStrategies[i].prop); return; }
            }
            Log(L"nenhuma estrategia funcionou para %llu", (unsigned long long)h);
        } else if (strategy >= 0) {
            HRESULT hr = vts->ClearProperty(h, strategyIdx);
            if (FAILED(hr)) Log(L"Restaurar(%llu) falhou: 0x%08lX", (unsigned long long)h, hr);
        }
    }
    void SetAll(bool want) {
        if (on == want) return;
        on = want;
        Log(L"SetAll(%d): %u elementos", want, (unsigned)elems.size());
        for (InstanceHandle h : elems) Apply(h, want);
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) { SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams); return TRUE; }
        Tap *t = (Tap *)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_TAP_SET && t) { t->SetAll(w != 0); return 1; }
        return DefWindowProcW(h, m, w, l);
    }
    void CreateControlWindow() {
        // Nome da classe = "PainelLeveTapWnd:" + nome do arquivo da DLL, para que cada versao da DLL
        // carregada no Explorer tenha a sua propria janela (o Explorer nao descarrega DLLs antigas).
        wchar_t cls[MAX_PATH + 32], path[MAX_PATH]; GetModuleFileNameW(g_mod, path, MAX_PATH);
        const wchar_t *base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        _snwprintf(cls, MAX_PATH + 32, L"PainelLeveTapWnd:%s", base);
        WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = g_mod; wc.lpszClassName = cls;
        RegisterClassW(&wc);
        wnd = CreateWindowExW(0, cls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_mod, this);
        Log(L"janela de controle '%s': %p (thread %lu)", cls, (void *)wnd, GetCurrentThreadId());
    }
};

struct Factory : IClassFactory {
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        Tap *t = new Tap(); HRESULT hr = t->QueryInterface(riid, ppv); t->Release(); return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

extern "C" {
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
    if (rclsid != CLSID_PainelLeveTap) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}
HRESULT WINAPI DllCanUnloadNow(void) { return g_objs ? S_FALSE : S_OK; }
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_mod = inst; DisableThreadLibraryCalls(inst); }
    return TRUE;
}
}
