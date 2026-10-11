// RAW - interface sur Mac : page web affichée par WKWebView (WebKit, fourni par macOS)
// Compilé avec ARC (-fobjc-arc).
#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "gui.h"
#include <string>
#include <memory>

namespace raw { class MacUi; }

// Nom de classe Objective-C propre à RAW : deux plugins chargés dans le même DAW
// ne doivent jamais déclarer la même classe.
@interface RAWSynthScriptHandler_0_3 : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) raw::MacUi* owner;
@end

namespace raw {

class MacUi final : public WebUi {
public:
    WKWebView* wv = nil;
    NSTimer* timer = nil;
    RAWSynthScriptHandler_0_3* handler = nil;
    MsgFn onMsg;
    TickFn onTick;

    ~MacUi() override {
        [timer invalidate];
        timer = nil;
        handler.owner = nullptr;
        [wv.configuration.userContentController removeScriptMessageHandlerForName:@"raw"];
        [wv removeFromSuperview];
        wv = nil;
        handler = nil;
    }

    bool attach(void* parent, int w, int h, const char* src, size_t len, MsgFn m, TickFn t) override {
        onMsg = std::move(m);
        onTick = std::move(t);
        NSView* pv = (__bridge NSView*)parent;
        if (!pv) return false;

        WKWebViewConfiguration* cfg = [[WKWebViewConfiguration alloc] init];
        handler = [[RAWSynthScriptHandler_0_3 alloc] init];
        handler.owner = this;
        [cfg.userContentController addScriptMessageHandler:handler name:@"raw"];

        wv = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, w, h) configuration:cfg];
        wv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [pv addSubview:wv];

        NSString* page = [[NSString alloc] initWithBytes:src length:len encoding:NSUTF8StringEncoding];
        [wv loadHTMLString:page baseURL:nil];

        MacUi* me = this;
        timer = [NSTimer timerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer*) {
            if (me->onTick) me->onTick();
        }];
        // Mode "common" : l'affichage continue pendant qu'on fait glisser un réglage
        [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
        return wv != nil;
    }

    void setSize(int w, int h) override { [wv setFrame:NSMakeRect(0, 0, w, h)]; }

    void eval(const std::string& js) override {
        if (!wv) return;
        NSString* s = [[NSString alloc] initWithBytes:js.data() length:js.size() encoding:NSUTF8StringEncoding];
        [wv evaluateJavaScript:s completionHandler:nil];
    }

    void show(bool visible) override { [wv setHidden:!visible]; }
};

std::unique_ptr<WebUi> WebUi::create() { return std::make_unique<MacUi>(); }

} // namespace raw

@implementation RAWSynthScriptHandler_0_3
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
    raw::MacUi* o = self.owner;
    if (!o || !o->onMsg) return;
    if ([message.body isKindOfClass:[NSString class]]) {
        const char* utf8 = [(NSString*)message.body UTF8String];
        if (utf8) o->onMsg(std::string(utf8));
    }
}
@end
#endif
