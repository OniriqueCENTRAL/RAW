// RAW - fenêtre d'interface : une page web affichée par le navigateur intégré au système
// (WebView2 sur Windows, WKWebView sur Mac). Implémentations dans gui_win.cpp et gui_mac.mm.
#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace raw {

class WebUi {
public:
    using MsgFn = std::function<void(const std::string&)>;
    using TickFn = std::function<void()>;

    // nullptr si la plateforme n'est pas prise en charge
    static std::unique_ptr<WebUi> create();
    virtual ~WebUi() = default;

    // parent : HWND (Windows) ou NSView* (Mac). onTick est appelé ~30 fois par seconde
    // sur le thread principal ; onMsg reçoit les messages envoyés par la page.
    virtual bool attach(void* parent, int w, int h, const char* html, size_t len, MsgFn onMsg, TickFn onTick) = 0;
    virtual void setSize(int w, int h) = 0;
    virtual void eval(const std::string& js) = 0;
    virtual void show(bool visible) = 0;
};

} // namespace raw
