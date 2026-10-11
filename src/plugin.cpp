// RAW - glue CLAP : paramètres, notes, audio, sauvegarde, latence et interface.
// Les formats VST3 et AU sont produits à partir de ce plugin CLAP par clap-wrapper.
#include <clap/clap.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include "dsp.h"
#include "gui.h"
#include "params.h"
#include "ui_html.h"

using namespace raw;

namespace {

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                           CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDesc = {
    CLAP_VERSION_INIT,
    "com.raw.synth",
    "RAW",
    "RAW",
    "",
    "",
    "",
    "0.3.0",
    "Synthe wavetable a deux oscillateurs pilote par un jeu de la vie",
    kFeatures,
};

#if defined(_WIN32)
const char* kGuiApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
const char* kGuiApi = CLAP_WINDOW_API_COCOA;
#elif defined(RAW_TEST_GUI)
const char* kGuiApi = "test"; // uniquement pour les tests automatiques
#else
const char* kGuiApi = nullptr;
#endif

// Messages de l'interface vers le thread audio
enum GuiEvType : uint8_t { GE_BEGIN, GE_VALUE, GE_END, GE_NOTEON, GE_NOTEOFF, GE_LIFERESET, GE_FREEZE };
struct GuiEv { GuiEvType type; uint32_t id; double v; };

struct RawPlugin {
    static constexpr int BASE_W = 1100, BASE_H = 760, MIN_W = 880, MIN_H = 660;
    static constexpr uint32_t SCOPE_N = 2048;

    clap_plugin_t plugin{};
    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_latency_t* hostLatency = nullptr;

    std::atomic<double> vals[kNumParams];
    Engine eng;
    Snap cur;
    int gi[7];
    struct FxMap { int ix, f, k; };
    FxMap fxMap[kNumParams];
    int nFx = 0;
    std::atomic<float> peak{0};
    std::atomic<bool> processing{false}, guiOpen{false};

    // Interface
    Spsc<GuiEv, 1024> fromGui;
    std::atomic<float> scope[SCOPE_N];
    std::atomic<uint32_t> scopeW{0};
    std::atomic<float> rms{0};
    std::unique_ptr<WebUi> ui;
    double lastSent[kNumParams];
    double scale = 1;
    uint32_t uiW = BASE_W, uiH = BASE_H;
    bool userSized = false;

    explicit RawPlugin(const clap_host_t* h) : host(h) {
        for (uint32_t i = 0; i < kNumParams; ++i) { vals[i].store(kParams[i].def); lastSent[i] = std::nan(""); }
        for (auto& x : scope) x.store(0, std::memory_order_relaxed);
        const uint32_t g[7] = {P_MASTER, P_LIFEAMT, P_LIFESPD, P_LIFESPAWN, P_MIX, P_LIFETGT, P_LIFERULE};
        for (int i = 0; i < 7; ++i) gi[i] = indexOf(g[i]);
        for (uint32_t i = 0; i < kNumParams; ++i)
            if (kParams[i].id >= FX_BASE) fxMap[nFx++] = {int(i), int((kParams[i].id - FX_BASE) / 10), int(kParams[i].id % 10)};
    }

    double val(int i) const { return vals[gi[i]].load(std::memory_order_relaxed); }

    void snap(Snap& s) const {
        for (int X = 0; X < 2; ++X)
            for (int k = 0; k < OSC_NP; ++k) s.o[X][k] = vals[X * OSC_NP + k].load(std::memory_order_relaxed);
        s.master = val(0); s.lifeAmt = val(1); s.lifeSpd = val(2); s.lifeSpawn = val(3);
        s.mix = int(std::lround(val(4))); s.tgt = int(std::lround(val(5))); s.rule = int(std::lround(val(6)));
        for (int i = 0; i < nFx; ++i) s.fx[fxMap[i].f][fxMap[i].k] = vals[fxMap[i].ix].load(std::memory_order_relaxed);
    }

    // --- thread audio --------------------------------------------------------
    void handle(const clap_event_header_t* h, uint32_t frames) {
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        eng.now = frames ? std::min(h->time, frames - 1) : 0;
        switch (h->type) {
            case CLAP_EVENT_NOTE_ON: {
                auto* e = reinterpret_cast<const clap_event_note_t*>(h);
                eng.noteOn(e->key, e->channel, e->note_id, e->velocity, cur);
                break;
            }
            case CLAP_EVENT_NOTE_OFF: {
                auto* e = reinterpret_cast<const clap_event_note_t*>(h);
                eng.noteOff(e->key, e->channel, e->note_id, cur);
                break;
            }
            case CLAP_EVENT_NOTE_CHOKE: {
                auto* e = reinterpret_cast<const clap_event_note_t*>(h);
                eng.choke(e->key, e->channel, e->note_id);
                break;
            }
            case CLAP_EVENT_PARAM_VALUE: {
                auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
                const int ix = indexOf(e->param_id);
                if (ix >= 0) { vals[ix].store(clampParam(ix, e->value)); snap(cur); }
                break;
            }
            case CLAP_EVENT_MIDI: {
                auto* e = reinterpret_cast<const clap_event_midi_t*>(h);
                const uint8_t status = e->data[0] & 0xF0, ch = e->data[0] & 0x0F;
                if (status == 0x90 && e->data[2] > 0) eng.noteOn(e->data[1], ch, -1, e->data[2] / 127.0, cur);
                else if (status == 0x80 || (status == 0x90 && e->data[2] == 0)) eng.noteOff(e->data[1], ch, -1, cur);
                break;
            }
            default: break;
        }
    }

    // Applique les actions de l'interface et les signale au DAW (pour l'enregistrement d'automation)
    void drainGui(const clap_output_events_t* out, bool notes) {
        GuiEv g;
        eng.now = 0;
        while (fromGui.pop(g)) {
            switch (g.type) {
                case GE_BEGIN:
                case GE_END: {
                    clap_event_param_gesture_t ev{};
                    ev.header.size = sizeof(ev);
                    ev.header.time = 0;
                    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    ev.header.type = uint16_t(g.type == GE_BEGIN ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END);
                    ev.header.flags = 0;
                    ev.param_id = g.id;
                    if (out) out->try_push(out, &ev.header);
                    break;
                }
                case GE_VALUE: {
                    const int ix = indexOf(g.id);
                    if (ix < 0) break;
                    vals[ix].store(g.v);
                    snap(cur);
                    clap_event_param_value_t ev{};
                    ev.header.size = sizeof(ev);
                    ev.header.time = 0;
                    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    ev.header.type = CLAP_EVENT_PARAM_VALUE;
                    ev.header.flags = 0;
                    ev.param_id = g.id;
                    ev.cookie = nullptr;
                    ev.note_id = -1;
                    ev.port_index = -1;
                    ev.channel = -1;
                    ev.key = -1;
                    ev.value = g.v;
                    if (out) out->try_push(out, &ev.header);
                    break;
                }
                case GE_NOTEON: if (notes) eng.noteOn(int(g.id), 0, kUiNoteId, g.v, cur); break;
                case GE_NOTEOFF: if (notes) eng.noteOff(int(g.id), 0, -1, cur); break;
                case GE_LIFERESET: eng.clearLife(); break;
                case GE_FREEZE: eng.frozen = g.v > .5; break;
            }
        }
    }

    void flushEnds(const clap_output_events_t* out, uint32_t frames) {
        for (int i = 0; i < eng.nEnds; ++i) {
            clap_event_note_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = frames ? std::min(eng.ends[i].time, frames - 1) : 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_NOTE_END;
            ev.header.flags = 0;
            ev.note_id = eng.ends[i].nid;
            ev.port_index = 0;
            ev.channel = int16_t(eng.ends[i].ch);
            ev.key = int16_t(eng.ends[i].key);
            ev.velocity = 0;
            if (out) out->try_push(out, &ev.header);
        }
        eng.nEnds = 0;
    }

    void feedScope(const float* L, const float* R, uint32_t n) {
        uint32_t w = scopeW.load(std::memory_order_relaxed);
        double sum = 0;
        float pk = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const float m = (L[i] + R[i]) * .5f;
            scope[(w + i) & (SCOPE_N - 1)].store(m, std::memory_order_relaxed);
            sum += double(m) * m;
            pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
        peak.store(pk, std::memory_order_relaxed);
        scopeW.store(w + n, std::memory_order_release);
        if (n) {
            const float r = float(std::sqrt(sum / n));
            const float old = rms.load(std::memory_order_relaxed);
            rms.store(old + (r - old) * .3f, std::memory_order_relaxed);
        }
    }

    // --- thread principal : interface ---------------------------------------
    void requestFlush() {
        if (!processing.load() && hostParams) hostParams->request_flush(host);
    }

    void onUiMessage(const std::string& m) {
        if (m.empty()) return;
        const char c = m[0];
        const char* s = m.c_str() + 1;
        double a = 0, b = 0;
        switch (c) {
            case 'R': // la page vient de se charger : tout renvoyer
                for (auto& x : lastSent) x = std::nan("");
                break;
            case 'b':
            case 'e':
                if (parseNumber(s, a) && indexOf(uint32_t(a)) >= 0)
                    fromGui.push({c == 'b' ? GE_BEGIN : GE_END, uint32_t(a), 0});
                break;
            case 'p':
                if (const char* r = parseNumber(s, a)) {
                    const int ix = indexOf(uint32_t(a));
                    if (ix >= 0 && parseNumber(r, b)) {
                        const double v = clampParam(ix, b);
                        vals[ix].store(v);
                        lastSent[ix] = v;
                        fromGui.push({GE_VALUE, uint32_t(a), v});
                    }
                }
                break;
            case 'n':
                if (const char* r = parseNumber(s, a))
                    if (parseNumber(r, b)) fromGui.push({GE_NOTEON, uint32_t(std::clamp(a, 0.0, 127.0)), std::clamp(b, 0.0, 1.0)});
                break;
            case 'o':
                if (parseNumber(s, a)) fromGui.push({GE_NOTEOFF, uint32_t(std::clamp(a, 0.0, 127.0)), 0});
                break;
            case 'z': fromGui.push({GE_LIFERESET, 0, 0}); break;
            case 'f': if (parseNumber(s, a)) fromGui.push({GE_FREEZE, 0, a}); break;
            default: return;
        }
        requestFlush();
    }

    // ~30 fois par seconde : envoie à la page les valeurs changées et les données à afficher
    void uiTick() {
        if (!ui) return;
        std::string js;
        js.reserve(9000);
        char b[64];
        js += "window.__rawRecv&&window.__rawRecv({\"p\":[";
        bool first = true;
        for (uint32_t i = 0; i < kNumParams; ++i) {
            const double v = vals[i].load(std::memory_order_relaxed);
            if (v == lastSent[i]) continue; // NaN : jamais égal, donc envoi forcé
            lastSent[i] = v;
            std::snprintf(b, sizeof(b), "%s[%u,%lld]", first ? "" : ",", unsigned(kParams[i].id), (long long)std::llround(v * 1e6));
            js += b;
            first = false;
        }
        js += "],\"s\":[";
        const uint32_t w = scopeW.load(std::memory_order_acquire);
        for (uint32_t k = 0; k < 512; ++k) {
            const float x = scope[(w - 512 + k) & (SCOPE_N - 1)].load(std::memory_order_relaxed);
            std::snprintf(b, sizeof(b), "%s%d", k ? "," : "", int(std::lround(std::clamp(x, -1.f, 1.f) * 1000)));
            js += b;
        }
        js += "],\"l\":\"";
        for (auto& c : eng.lifeView) js += char('0' + std::min<int>(9, c.load(std::memory_order_relaxed)));
        std::snprintf(b, sizeof(b), "\",\"d\":%d,\"r\":%d,\"n\":[",
                      int(std::lround(eng.densView.load() * 1000)), int(std::lround(rms.load() * 1000)));
        js += b;
        NoteVis nv;
        first = true;
        while (eng.visNotes.pop(nv)) {
            std::snprintf(b, sizeof(b), "%s[%d,%d]", first ? "" : ",", nv.key, int(std::lround(nv.vel * 100)));
            js += b;
            first = false;
        }
        js += "]})";
        ui->eval(js);
    }
};

RawPlugin* self(const clap_plugin_t* p) { return static_cast<RawPlugin*>(p->plugin_data); }

// --- paramètres --------------------------------------------------------------
uint32_t paramsCount(const clap_plugin_t*) { return kNumParams; }

bool paramsInfo(const clap_plugin_t*, uint32_t i, clap_param_info_t* info) {
    if (i >= kNumParams) return false;
    const ParamDef& d = kParams[i];
    std::memset(info, 0, sizeof(*info));
    info->id = d.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | (d.choices ? CLAP_PARAM_IS_STEPPED : 0);
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof(info->name), "%s", d.name);
    std::snprintf(info->module, sizeof(info->module), "%s", d.module);
    info->min_value = 0;
    info->max_value = d.choices ? d.choices - 1 : 1;
    info->default_value = d.def;
    return true;
}

bool paramsValue(const clap_plugin_t* p, clap_id id, double* out) {
    const int ix = indexOf(id);
    if (ix < 0) return false;
    *out = self(p)->vals[ix].load();
    return true;
}

bool paramsToText(const clap_plugin_t*, clap_id id, double v, char* out, uint32_t cap) {
    const int ix = indexOf(id);
    if (ix < 0 || cap == 0) return false;
    formatParam(kParams[ix], v, out, cap);
    return true;
}

bool paramsFromText(const clap_plugin_t*, clap_id id, const char* txt, double* out) {
    const int ix = indexOf(id);
    if (ix < 0) return false;
    double v = 0;
    if (!parseParamText(kParams[ix], txt, v)) return false;
    *out = v;
    return true;
}

void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
    auto* me = self(p);
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) me->handle(h, 0);
    }
    me->drainGui(out, false);
}

const clap_plugin_params_t kParamsExt = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush};

// --- état (sauvegarde dans le projet du DAW) --------------------------------
bool writeAll(const clap_ostream_t* s, const void* buf, uint64_t size) {
    const char* p = static_cast<const char*>(buf);
    while (size > 0) {
        const int64_t w = s->write(s, p, size);
        if (w <= 0) return false;
        p += w; size -= uint64_t(w);
    }
    return true;
}
bool readAll(const clap_istream_t* s, void* buf, uint64_t size) {
    char* p = static_cast<char*>(buf);
    while (size > 0) {
        const int64_t r = s->read(s, p, size);
        if (r <= 0) return false;
        p += r; size -= uint64_t(r);
    }
    return true;
}
constexpr uint32_t kMagic = 0x52415731; // "RAW1"

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) {
    auto* me = self(p);
    const uint32_t n = kNumParams;
    if (!writeAll(s, &kMagic, 4) || !writeAll(s, &n, 4)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = kParams[i].id;
        const double v = me->vals[i].load();
        if (!writeAll(s, &id, 4) || !writeAll(s, &v, 8)) return false;
    }
    return true;
}

bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) {
    auto* me = self(p);
    uint32_t magic = 0, n = 0;
    if (!readAll(s, &magic, 4) || magic != kMagic || !readAll(s, &n, 4)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = 0;
        double v = 0;
        if (!readAll(s, &id, 4) || !readAll(s, &v, 8)) return false;
        const int ix = indexOf(id);
        if (ix >= 0) me->vals[ix].store(clampParam(ix, v));
    }
    // Pas d'accès à me->cur ici : le thread audio le relit au début de chaque bloc.
    if (me->hostParams) me->hostParams->rescan(me->host, CLAP_PARAM_RESCAN_VALUES);
    return true;
}

const clap_plugin_state_t kStateExt = {stateSave, stateLoad};

// --- latence (limiteur à anticipation) --------------------------------------
uint32_t latencyGet(const clap_plugin_t* p) { return self(p)->eng.latency(); }
const clap_plugin_latency_t kLatencyExt = {latencyGet};

// --- ports audio et notes ---------------------------------------------------
uint32_t audioCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 1; }
bool audioGet(const clap_plugin_t*, uint32_t i, bool isInput, clap_audio_port_info_t* info) {
    if (isInput || i != 0) return false;
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", "sortie");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kAudioPorts = {audioCount, audioGet};

uint32_t noteCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
bool noteGet(const clap_plugin_t*, uint32_t i, bool isInput, clap_note_port_info_t* info) {
    if (!isInput || i != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "%s", "notes");
    return true;
}
const clap_plugin_note_ports_t kNotePorts = {noteCount, noteGet};

// --- interface ---------------------------------------------------------------
bool guiSupported(const clap_plugin_t*, const char* api, bool floating) {
    return !floating && kGuiApi && api && std::strcmp(api, kGuiApi) == 0;
}
bool guiPreferred(const clap_plugin_t*, const char** api, bool* floating) {
    if (!kGuiApi) return false;
    *api = kGuiApi;
    *floating = false;
    return true;
}
bool guiCreate(const clap_plugin_t* p, const char* api, bool floating) {
    if (!guiSupported(p, api, floating)) return false;
    auto* me = self(p);
    me->ui = WebUi::create();
    return me->ui != nullptr;
}
void guiDestroy(const clap_plugin_t* p) {
    auto* me = self(p);
    me->guiOpen.store(false);
    me->ui.reset();
}
bool guiSetScale(const clap_plugin_t* p, double scale) {
#if defined(_WIN32)
    auto* me = self(p);
    me->scale = scale > 0 ? scale : 1;
    if (!me->userSized) {
        me->uiW = uint32_t(std::lround(RawPlugin::BASE_W * me->scale));
        me->uiH = uint32_t(std::lround(RawPlugin::BASE_H * me->scale));
    }
    return true;
#else
    (void)p; (void)scale;
    return false; // sur Mac, le système gère l'échelle
#endif
}
bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    auto* me = self(p);
    *w = me->uiW;
    *h = me->uiH;
    return true;
}
bool guiCanResize(const clap_plugin_t*) { return true; }
bool guiResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    hints->aspect_ratio_width = 0;
    hints->aspect_ratio_height = 0;
    return true;
}
bool guiAdjust(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    auto* me = self(p);
    *w = std::max(*w, uint32_t(std::lround(RawPlugin::MIN_W * me->scale)));
    *h = std::max(*h, uint32_t(std::lround(RawPlugin::MIN_H * me->scale)));
    return true;
}
bool guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    auto* me = self(p);
    me->uiW = w;
    me->uiH = h;
    me->userSized = true;
    if (me->ui) me->ui->setSize(int(w), int(h));
    return true;
}
bool guiSetParent(const clap_plugin_t* p, const clap_window_t* win) {
    auto* me = self(p);
    if (!me->ui || !win) return false;
    me->eng.visNotes.clear(); // pas de rafale d'animations à l'ouverture
    for (auto& x : me->lastSent) x = std::nan("");
    const bool ok = me->ui->attach(win->ptr, int(me->uiW), int(me->uiH),
                                   reinterpret_cast<const char*>(kUiHtml), kUiHtmlLen,
                                   [me](const std::string& m) { me->onUiMessage(m); },
                                   [me] { me->uiTick(); });
    me->guiOpen.store(ok);
    return ok;
}
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t* p) {
    auto* me = self(p);
    if (me->ui) me->ui->show(true);
    return me->ui != nullptr;
}
bool guiHide(const clap_plugin_t* p) {
    auto* me = self(p);
    if (me->ui) me->ui->show(false);
    return me->ui != nullptr;
}
const clap_plugin_gui_t kGuiExt = {guiSupported, guiPreferred, guiCreate, guiDestroy, guiSetScale,
                                   guiGetSize, guiCanResize, guiResizeHints, guiAdjust, guiSetSize,
                                   guiSetParent, guiSetTransient, guiSuggestTitle, guiShow, guiHide};

// --- cycle de vie -----------------------------------------------------------
bool pInit(const clap_plugin_t* p) {
    auto* me = self(p);
    me->hostParams = static_cast<const clap_host_params_t*>(me->host->get_extension(me->host, CLAP_EXT_PARAMS));
    me->hostLatency = static_cast<const clap_host_latency_t*>(me->host->get_extension(me->host, CLAP_EXT_LATENCY));
    return true;
}
void pDestroy(const clap_plugin_t* p) { delete self(p); }
bool pActivate(const clap_plugin_t* p, double sr, uint32_t, uint32_t) {
    auto* me = self(p);
    bank(); // construit la banque wavetable hors du thread audio
    me->eng.init(sr);
    me->snap(me->cur);
    if (me->hostLatency) me->hostLatency->changed(me->host); // la latence dépend de la fréquence
    return true;
}
void pDeactivate(const clap_plugin_t*) {}
bool pStart(const clap_plugin_t* p) { self(p)->processing.store(true); return true; }
void pStop(const clap_plugin_t* p) { self(p)->processing.store(false); }
void pReset(const clap_plugin_t* p) { self(p)->eng.reset(); }

clap_process_status pProcess(const clap_plugin_t* p, const clap_process_t* pr) {
    auto* me = self(p);
    NoDenormals noDenormals;
    if (pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 2 || !pr->audio_outputs[0].data32)
        return CLAP_PROCESS_CONTINUE;
    float* L = pr->audio_outputs[0].data32[0];
    float* R = pr->audio_outputs[0].data32[1];
    pr->audio_outputs[0].constant_mask = 0;
    const uint32_t n = pr->frames_count;
    const clap_input_events_t* in = pr->in_events;
    const uint32_t ne = in ? in->size(in) : 0;

    me->snap(me->cur);
    me->drainGui(pr->out_events, true); // événements à l'instant 0, avant les fins de notes

    uint32_t ei = 0, i = 0;
    while (i < n) {
        while (ei < ne) {
            const clap_event_header_t* h = in->get(in, ei);
            if (h->time > i) break;
            me->handle(h, n);
            ++ei;
        }
        uint32_t next = n;
        if (ei < ne) {
            const uint32_t t = in->get(in, ei)->time;
            if (t < n) next = t;
        }
        me->eng.render(L + i, R + i, next - i, me->cur, i);
        i = next;
    }
    while (ei < ne) me->handle(in->get(in, ei++), n);

    me->feedScope(L, R, n);
    me->flushEnds(pr->out_events, n);
    // En veille si rien ne joue, que la queue des effets (delay, reverb) est éteinte
    // et que l'interface est fermée. Le DAW nous réveille à la prochaine note.
    if (!me->eng.anyActive() && !me->guiOpen.load() && me->peak.load() < 1e-5f) return CLAP_PROCESS_SLEEP;
    return CLAP_PROCESS_CONTINUE;
}

const void* pGetExt(const clap_plugin_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParamsExt;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kStateExt;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatencyExt;
    if (!std::strcmp(id, CLAP_EXT_GUI) && kGuiApi) return &kGuiExt;
    return nullptr;
}
void pOnMain(const clap_plugin_t*) {}

// --- fabrique ---------------------------------------------------------------
uint32_t fCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &kDesc : nullptr; }
const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, kDesc.id) != 0) return nullptr;
    auto* me = new RawPlugin(host);
    clap_plugin_t& pl = me->plugin;
    pl.desc = &kDesc;
    pl.plugin_data = me;
    pl.init = pInit;
    pl.destroy = pDestroy;
    pl.activate = pActivate;
    pl.deactivate = pDeactivate;
    pl.start_processing = pStart;
    pl.stop_processing = pStop;
    pl.reset = pReset;
    pl.process = pProcess;
    pl.get_extension = pGetExt;
    pl.on_main_thread = pOnMain;
    return &me->plugin;
}
const clap_plugin_factory_t kFactory = {fCount, fDesc, fCreate};

} // namespace

// Points d'entrée utilisés par entry.cpp
bool raw_entry_init(const char*) { return true; }
void raw_entry_deinit() {}
const void* raw_entry_get_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}
