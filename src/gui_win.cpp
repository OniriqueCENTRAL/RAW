// RAW - interface sous Windows : page web affichée par WebView2 (Edge intégré à Windows 10/11)
#if defined(_WIN32)
#include "gui.h"
#include <memory>

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace raw {
namespace {

std::wstring widen(const char* s, size_t n) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, int(n), nullptr, 0);
    std::wstring w(size_t(len > 0 ? len : 0), L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s, int(n), w.data(), len);
    return w;
}

std::string narrow(const wchar_t* w) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(size_t(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// Nom de classe propre à RAW : évite les collisions avec d'autres plugins chargés dans le même DAW
const wchar_t* kClass = L"RAWSynthWebHost_0_3";
int gWindows = 0; // nombre de fenêtres ouvertes, pour désenregistrer la classe à la fin

class WinUi final : public WebUi {
public:
    ~WinUi() override {
        *alive = false;
        if (hwnd) {
            KillTimer(hwnd, 1);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        if (wv) wv->remove_WebMessageReceived(token);
        if (ctl) ctl->Close();
        wv.Reset();
        ctl.Reset();
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
            // Si la DLL est déchargée puis rechargée ailleurs en mémoire, une classe restée
            // enregistrée pointerait vers du code disparu : on la retire avec la dernière fenêtre.
            if (--gWindows == 0) UnregisterClassW(kClass, module());
        }
    }

    bool attach(void* parent, int w, int h, const char* src, size_t len, MsgFn m, TickFn t) override {
        onMsg = std::move(m);
        onTick = std::move(t);
        html = widen(src, len);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        if (!GetClassInfoExW(module(), kClass, &wc)) {
            wc = WNDCLASSEXW{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &WinUi::proc;
            wc.hInstance = module();
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = kClass;
            if (!RegisterClassExW(&wc)) return false;
        }
        hwnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, w, h,
                               static_cast<HWND>(parent), nullptr, module(), nullptr);
        if (!hwnd) return false;
        ++gWindows;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetTimer(hwnd, 1, 33, nullptr);

        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // sans effet si déjà fait par le DAW

        // Dossier de données dans le profil utilisateur : par défaut WebView2 écrirait
        // à côté du .exe du DAW, souvent dans Program Files (interdit en écriture).
        std::wstring dataDir;
        wchar_t base[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n > 0 && n < MAX_PATH) dataDir = std::wstring(base) + L"\\RAW\\WebView2";

        auto guard = alive;
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, dataDir.empty() ? nullptr : dataDir.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, guard](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                    if (!*guard || FAILED(res) || !env) return S_OK; // fenêtre déjà fermée entre-temps
                    return env->CreateCoreWebView2Controller(
                        hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                  [this, guard](HRESULT res2, ICoreWebView2Controller* c) -> HRESULT {
                                      if (!*guard || FAILED(res2) || !c) return S_OK;
                                      onReady(c);
                                      return S_OK;
                                  }).Get());
                }).Get());
        return SUCCEEDED(hr);
    }

    void setSize(int w, int h) override {
        if (hwnd) SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }

    void eval(const std::string& js) override {
        if (!wv) return; // page pas encore prête
        const std::wstring w = widen(js.data(), js.size());
        wv->ExecuteScript(w.c_str(), nullptr);
    }

    void show(bool visible) override {
        if (hwnd) ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
        if (ctl) ctl->put_IsVisible(visible ? TRUE : FALSE);
    }

private:
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> ctl;
    ComPtr<ICoreWebView2> wv;
    EventRegistrationToken token{};
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::wstring html;
    MsgFn onMsg;
    TickFn onTick;

    static HMODULE module() {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&WinUi::proc), &m);
        return m;
    }

    void onReady(ICoreWebView2Controller* c) {
        ctl = c;
        if (FAILED(ctl->get_CoreWebView2(&wv)) || !wv) return;
        RECT rc;
        GetClientRect(hwnd, &rc);
        ctl->put_Bounds(rc);
        ComPtr<ICoreWebView2Settings> st;
        if (SUCCEEDED(wv->get_Settings(&st)) && st) {
            st->put_AreDevToolsEnabled(FALSE);
            st->put_AreDefaultContextMenusEnabled(FALSE);
            st->put_IsStatusBarEnabled(FALSE);
            st->put_IsZoomControlEnabled(FALSE);
        }
        auto guard = alive;
        wv->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this, guard](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    if (!*guard) return S_OK;
                    LPWSTR s = nullptr;
                    if (SUCCEEDED(args->TryGetWebMessageAsString(&s)) && s) {
                        if (onMsg) onMsg(narrow(s));
                        CoTaskMemFree(s);
                    }
                    return S_OK;
                }).Get(),
            &token);
        wv->NavigateToString(html.c_str());
    }

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* me = reinterpret_cast<WinUi*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (me) {
            if (msg == WM_SIZE && me->ctl) {
                RECT rc;
                GetClientRect(h, &rc);
                me->ctl->put_Bounds(rc);
            } else if (msg == WM_TIMER && wp == 1 && me->onTick) {
                me->onTick();
                return 0;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

} // namespace

std::unique_ptr<WebUi> WebUi::create() { return std::make_unique<WinUi>(); }

} // namespace raw
#endif
