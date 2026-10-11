// RAW - définition des paramètres (mêmes valeurs normalisées 0..1 que la maquette HTML)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace raw {

// Paramètres d'un oscillateur (identiques pour A et B)
enum OscP { POS, LVL, SEMI, UNI, DET, PAN, ATK, DEC, SUS, REL, CUT, RES, LFOR, LFOD, OSC_NP };

enum Fmt { F_TAB, F_PCT, F_SEMI, F_UNI, F_DET, F_PAN, F_TIME, F_HZ, F_Q, F_RATE, F_MS, F_MIX, F_TGT, F_RULE,
           F_ONOFF, F_FXRATE, F_TONE, F_DLY, F_THR, F_RATIO, F_MAKEUP, F_EQ, F_FTYPE, F_SIZE, F_DAMP };

struct ParamDef {
    uint32_t id;
    const char* name;
    const char* module;
    double def;
    int choices; // 0 = continu (0..1), sinon nombre de choix (0..choices-1)
    Fmt fmt;
};

// Conversions, reprises telles quelles de la maquette
inline double st(double v) { return std::round((v - .5) * 480) / 10; }  // demi-tons
inline double tm(double v) { return .003 + v * v * 3; }                 // secondes
inline double hz(double v) { return 80 * std::pow(150.0, v); }          // cutoff
inline double qq(double v) { return .5 + v * v * 18; }                  // Q
inline double lr(double v) { return .1 * std::pow(150.0, v); }          // vitesse LFO
inline double spd(double v) { return 400 - v * 340; }                   // ms entre deux générations

constexpr uint32_t OSC_BASE[2] = {0, 20};
enum : uint32_t { P_MASTER = 100, P_LIFEAMT, P_LIFESPD, P_LIFESPAWN, P_MIX, P_LIFETGT, P_LIFERULE };

#define RAW_OSC_PARAMS(B, NM, DPOS, DLVL, DSEMI)                                   \
    {B + POS, NM " position", NM, DPOS, 0, F_TAB}, {B + LVL, NM " level", NM, DLVL, 0, F_PCT}, \
    {B + SEMI, NM " tune", NM, DSEMI, 0, F_SEMI}, {B + UNI, NM " unison", NM, 0, 0, F_UNI},     \
    {B + DET, NM " detune", NM, .25, 0, F_DET}, {B + PAN, NM " pan", NM, .5, 0, F_PAN},          \
    {B + ATK, NM " attack", NM, .05, 0, F_TIME}, {B + DEC, NM " decay", NM, .35, 0, F_TIME},     \
    {B + SUS, NM " sustain", NM, .6, 0, F_PCT}, {B + REL, NM " release", NM, .3, 0, F_TIME},     \
    {B + CUT, NM " cutoff", NM, .8, 0, F_HZ}, {B + RES, NM " reso", NM, .2, 0, F_Q},             \
    {B + LFOR, NM " lfo rate", NM, .3, 0, F_RATE}, {B + LFOD, NM " lfo depth", NM, .1, 0, F_PCT}

inline constexpr ParamDef kParams[] = {
    RAW_OSC_PARAMS(0u, "osc a", 0.0, .7, .5),
    RAW_OSC_PARAMS(20u, "osc b", .5, .5, .502),
    {P_MASTER, "master", "general", .6, 0, F_PCT},
    {P_LIFEAMT, "vie amount", "vie", .5, 0, F_PCT},
    {P_LIFESPD, "vie vitesse", "vie", .5, 0, F_MS},
    {P_LIFESPAWN, "vie naissance", "vie", .5, 0, F_PCT},
    {P_MIX, "mix a/b", "general", 0, 3, F_MIX},
    {P_LIFETGT, "vie cible", "vie", 2, 4, F_TGT},
    {P_LIFERULE, "vie regle", "vie", 0, 3, F_RULE},
    // Effets : identifiant 200 + 10 x effet + n (n = 0 pour marche/arrêt)
    {200, "fx hyper on", "fx", 0, 2, F_ONOFF}, {201, "fx hyper rate", "fx", .3, 0, F_FXRATE},
    {202, "fx hyper detune", "fx", .4, 0, F_PCT}, {203, "fx hyper mix", "fx", .4, 0, F_PCT},
    {210, "fx dist on", "fx", 0, 2, F_ONOFF}, {211, "fx dist drive", "fx", .3, 0, F_PCT},
    {212, "fx dist tone", "fx", .7, 0, F_TONE}, {213, "fx dist mix", "fx", .6, 0, F_PCT},
    {220, "fx flanger on", "fx", 0, 2, F_ONOFF}, {221, "fx flanger rate", "fx", .25, 0, F_FXRATE},
    {222, "fx flanger depth", "fx", .5, 0, F_PCT}, {223, "fx flanger feedback", "fx", .4, 0, F_PCT},
    {224, "fx flanger mix", "fx", .5, 0, F_PCT},
    {230, "fx phaser on", "fx", 0, 2, F_ONOFF}, {231, "fx phaser rate", "fx", .2, 0, F_FXRATE},
    {232, "fx phaser depth", "fx", .6, 0, F_PCT}, {233, "fx phaser mix", "fx", .5, 0, F_PCT},
    {240, "fx chorus on", "fx", 0, 2, F_ONOFF}, {241, "fx chorus rate", "fx", .2, 0, F_FXRATE},
    {242, "fx chorus depth", "fx", .5, 0, F_PCT}, {243, "fx chorus mix", "fx", .4, 0, F_PCT},
    {250, "fx delay on", "fx", 0, 2, F_ONOFF}, {251, "fx delay time", "fx", .4, 0, F_DLY},
    {252, "fx delay feedback", "fx", .4, 0, F_PCT}, {253, "fx delay mix", "fx", .35, 0, F_PCT},
    {260, "fx comp on", "fx", 0, 2, F_ONOFF}, {261, "fx comp thresh", "fx", .5, 0, F_THR},
    {262, "fx comp ratio", "fx", .4, 0, F_RATIO}, {263, "fx comp gain", "fx", .3, 0, F_MAKEUP},
    {270, "fx eq on", "fx", 0, 2, F_ONOFF}, {271, "fx eq low", "fx", .5, 0, F_EQ},
    {272, "fx eq mid", "fx", .5, 0, F_EQ}, {273, "fx eq high", "fx", .5, 0, F_EQ},
    {280, "fx filter on", "fx", 0, 2, F_ONOFF}, {281, "fx filter cutoff", "fx", .6, 0, F_HZ},
    {282, "fx filter reso", "fx", .2, 0, F_Q}, {283, "fx filter type", "fx", 0, 3, F_FTYPE},
    {290, "fx reverb on", "fx", 0, 2, F_ONOFF}, {291, "fx reverb size", "fx", .5, 0, F_SIZE},
    {292, "fx reverb damp", "fx", .4, 0, F_DAMP}, {293, "fx reverb mix", "fx", .3, 0, F_PCT},
};
inline constexpr uint32_t kNumParams = sizeof(kParams) / sizeof(kParams[0]);
static_assert(kParams[OSC_NP].id == 20, "l'osc B doit suivre l'osc A");

constexpr int indexOf(uint32_t id) {
    for (uint32_t i = 0; i < kNumParams; ++i)
        if (kParams[i].id == id) return int(i);
    return -1;
}

inline constexpr const char* kMixNames[] = {"add", "fm", "ring"};
inline constexpr const char* kTgtNames[] = {"off", "pos a", "pos b", "cutoff"};
inline constexpr const char* kRuleNames[] = {"conway", "highlife", "seeds"};
inline constexpr const char* kOnNames[] = {"off", "on"};
inline constexpr const char* kTypeNames[] = {"lp", "bp", "hp"};
constexpr uint32_t FX_BASE = 200;
constexpr int kNumFx = 10;

inline const char* const* choiceNames(Fmt f) {
    switch (f) {
        case F_MIX: return kMixNames;
        case F_TGT: return kTgtNames;
        case F_RULE: return kRuleNames;
        case F_ONOFF: return kOnNames;
        case F_FTYPE: return kTypeNames;
        default: return nullptr;
    }
}

inline double clampParam(int ix, double v) {
    const auto& d = kParams[ix];
    if (d.choices) return std::clamp(std::round(v), 0.0, double(d.choices - 1));
    return std::clamp(v, 0.0, 1.0);
}

inline void formatParam(const ParamDef& p, double v, char* out, uint32_t cap) {
    switch (p.fmt) {
        case F_TAB: std::snprintf(out, cap, "%d/64", int(std::lround(1 + v * 63))); break;
        case F_PCT: std::snprintf(out, cap, "%d%%", int(std::lround(v * 100))); break;
        case F_SEMI: std::snprintf(out, cap, "%+.1f st", st(v)); break;
        case F_UNI: std::snprintf(out, cap, "%d voix", 1 + int(std::lround(v * 4))); break;
        case F_DET: std::snprintf(out, cap, "%d cents", int(std::lround(v * 60))); break;
        case F_PAN: {
            int q = int(std::lround((v - .5) * 200));
            if (q == 0) std::snprintf(out, cap, "C");
            else std::snprintf(out, cap, "%c%d", q < 0 ? 'L' : 'R', std::abs(q));
            break;
        }
        case F_TIME: {
            double t = tm(v);
            if (t < 1) std::snprintf(out, cap, "%d ms", int(std::lround(t * 1000)));
            else std::snprintf(out, cap, "%.2f s", t);
            break;
        }
        case F_HZ: {
            double h = hz(v);
            if (h >= 1000) std::snprintf(out, cap, "%.1f kHz", h / 1000);
            else std::snprintf(out, cap, "%d Hz", int(std::lround(h)));
            break;
        }
        case F_Q: std::snprintf(out, cap, "Q %.1f", qq(v)); break;
        case F_RATE: std::snprintf(out, cap, "%.2f Hz", lr(v)); break;
        case F_MS: std::snprintf(out, cap, "%d ms", int(std::lround(spd(v)))); break;
        case F_MIX: case F_TGT: case F_RULE: case F_ONOFF: case F_FTYPE: {
            int i = std::clamp(int(std::lround(v)), 0, p.choices - 1);
            std::snprintf(out, cap, "%s", choiceNames(p.fmt)[i]);
            break;
        }
        case F_FXRATE: std::snprintf(out, cap, "%.2f Hz", .1 + v * 4); break;
        case F_TONE: std::snprintf(out, cap, "%.1f kHz", 600 * std::pow(25.0, v) / 1000); break;
        case F_DLY: std::snprintf(out, cap, "%d ms", int(std::lround((.03 + v * .97) * 1000))); break;
        case F_THR: std::snprintf(out, cap, "%d dB", int(std::lround(-60 + v * 60))); break;
        case F_RATIO: std::snprintf(out, cap, "%.1f:1", 1 + v * 19); break;
        case F_MAKEUP: std::snprintf(out, cap, "+%.1f dB", v * 12); break;
        case F_EQ: { const int x = int(std::lround((v - .5) * 24)); std::snprintf(out, cap, "%+d dB", x); break; }
        case F_SIZE: std::snprintf(out, cap, "%.1f s", .4 + v * 3.6); break;
        case F_DAMP: std::snprintf(out, cap, "%.1f kHz", (16000 - v * 14500) / 1000); break;
    }
}

// Lecture d'un nombre indépendante de la langue du système (accepte 0.5 et 0,5)
inline const char* parseNumber(const char* s, double& out) {
    if (!s) return nullptr;
    while (*s == ' ' || *s == '\t') ++s;
    bool neg = false;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); ++s; }
    double v = 0;
    bool any = false;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; any = true; }
    if (*s == '.' || *s == ',') {
        ++s;
        double f = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; ++s; any = true; }
    }
    if (!any) return nullptr;
    out = neg ? -v : v;
    return s;
}

inline bool sameWord(const char* a, const char* b) {
    while (*a == ' ') ++a;
    while (*b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (x != y) return false;
        ++a; ++b;
    }
    while (*a == ' ') ++a;
    return *a == 0;
}

// Texte tapé dans le DAW -> valeur normalisée. Comprend les unités affichées (Hz, kHz, ms, s, %, st...)
inline bool parseParamText(const ParamDef& p, const char* txt, double& out) {
    if (!txt) return false;
    while (*txt == ' ') ++txt;
    if (const char* const* names = choiceNames(p.fmt)) {
        for (int i = 0; i < p.choices; ++i)
            if (sameWord(txt, names[i])) { out = i; return true; }
    }
    double x = 0;
    const char* rest = nullptr;
    if (p.fmt == F_PAN) {
        const char c = txt[0];
        if (c == 'C' || c == 'c') { out = .5; return true; }
        if (c == 'L' || c == 'l' || c == 'R' || c == 'r') {
            if (!parseNumber(txt + 1, x)) return false;
            out = std::clamp(.5 + ((c == 'L' || c == 'l') ? -x : x) / 200, 0.0, 1.0);
            return true;
        }
    }
    if (p.fmt == F_Q && (txt[0] == 'Q' || txt[0] == 'q')) ++txt;
    rest = parseNumber(txt, x);
    if (!rest) return false;
    while (*rest == ' ') ++rest;
    const char u0 = char(rest[0] | 0x20);
    switch (p.fmt) {
        case F_TAB: out = (x - 1) / 63; break;
        case F_PCT: out = x / 100; break;
        case F_SEMI: out = .5 + x / 48; break;
        case F_UNI: out = (x - 1) / 4; break;
        case F_DET: out = x / 60; break;
        case F_PAN: out = .5 + x / 200; break;
        case F_TIME: {
            const double t = (u0 == 's') ? x : x / 1000; // sans unité : millisecondes
            out = std::sqrt(std::max(0.0, (t - .003) / 3));
            break;
        }
        case F_HZ: {
            const double h = (u0 == 'k') ? x * 1000 : x;
            out = std::log(std::max(h, 1e-9) / 80) / std::log(150.0);
            break;
        }
        case F_Q: out = std::sqrt(std::max(0.0, (x - .5) / 18)); break;
        case F_RATE: out = std::log(std::max(x, 1e-9) / .1) / std::log(150.0); break;
        case F_MS: out = (400 - x) / 340; break;
        case F_MIX: case F_TGT: case F_RULE: case F_ONOFF: case F_FTYPE: out = x; break;
        case F_FXRATE: out = (x - .1) / 4; break;
        case F_TONE: { const double h = (u0 == 'k') ? x * 1000 : x; out = std::log(std::max(h, 1e-9) / 600) / std::log(25.0); break; }
        case F_DLY: { const double t = (u0 == 's') ? x : x / 1000; out = (t - .03) / .97; break; }
        case F_THR: out = (x + 60) / 60; break;
        case F_RATIO: out = (x - 1) / 19; break;
        case F_MAKEUP: out = x / 12; break;
        case F_EQ: out = .5 + x / 24; break;
        case F_SIZE: out = (x - .4) / 3.6; break;
        case F_DAMP: { const double h = (u0 == 'k') ? x * 1000 : x; out = (16000 - h) / 14500; break; }
    }
    if (p.choices) out = std::clamp(std::round(out), 0.0, double(p.choices - 1));
    else out = std::clamp(out, 0.0, 1.0);
    return true;
}

} // namespace raw
