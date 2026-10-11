// RAW - moteur audio portable (aucune dépendance, C++20)
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "fx.h"
#include "params.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define RAW_X86 1
#endif

namespace raw {

constexpr double PI = 3.14159265358979323846;

// Coupe les nombres sous-normaux pendant le traitement audio (évite les pics de charge)
struct NoDenormals {
#if defined(RAW_X86)
    unsigned int old;
    NoDenormals() : old(_mm_getcsr()) { _mm_setcsr(old | 0x8040u); }
    ~NoDenormals() { _mm_setcsr(old); }
#elif defined(__aarch64__) && !defined(_MSC_VER)
    uint64_t old;
    NoDenormals() {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(old));
        const uint64_t n = old | (1ull << 24);
        __asm__ __volatile__("msr fpcr, %0" : : "r"(n));
    }
    ~NoDenormals() { __asm__ __volatile__("msr fpcr, %0" : : "r"(old)); }
#endif
};

// File sans verrou, un producteur et un consommateur
template <class T, size_t N>
class Spsc {
    static_assert((N & (N - 1)) == 0, "N doit être une puissance de 2");
    T buf[N];
    std::atomic<size_t> w{0}, r{0};
public:
    bool push(const T& v) {
        const size_t wi = w.load(std::memory_order_relaxed);
        if (wi - r.load(std::memory_order_acquire) >= N) return false;
        buf[wi & (N - 1)] = v;
        w.store(wi + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const size_t ri = r.load(std::memory_order_relaxed);
        if (ri == w.load(std::memory_order_acquire)) return false;
        v = buf[ri & (N - 1)];
        r.store(ri + 1, std::memory_order_release);
        return true;
    }
    void clear() { T t; while (pop(t)) {} } // côté consommateur uniquement
};

// ---------------------------------------------------------------------------
// Banque wavetable : 64 positions (sinus > triangle > dent de scie > carré),
// jusqu'à 256 harmoniques dans le grave, 9 niveaux pour éviter l'aliasing.
// ---------------------------------------------------------------------------
struct WaveBank {
    static constexpr int N = 2048, FR = 64, LV = 9, KMAX = 256;
    std::vector<float> d;

    static double shape(int i, int k) {
        switch (i) {
            case 0: return k == 1 ? 1.0 : 0.0;
            case 1: return (k % 2) ? ((((k - 1) / 2) % 2) ? -1.0 : 1.0) / double(k) / double(k) : 0.0;
            case 2: return 1.0 / k;
            default: return (k % 2) ? 1.0 / k : 0.0;
        }
    }
    static void harm(double p, double* h) {
        const double x = p * 3.0;
        const int i = std::min(2, int(std::floor(x)));
        const double f = x - i;
        h[0] = 0;
        for (int k = 1; k <= KMAX; ++k) h[k] = shape(i, k) * (1 - f) + shape(i + 1, k) * f;
    }

    void build() {
        d.assign(size_t(FR) * LV * N, 0.f);
        std::vector<double> sn(N), tmp(N), h(KMAX + 1);
        for (int i = 0; i < N; ++i) sn[i] = std::sin(2 * PI * i / N);
        for (int f = 0; f < FR; ++f) {
            harm(f / double(FR - 1), h.data());
            for (int l = 0; l < LV; ++l) {
                const int hm = KMAX >> l;
                double pk = 0;
                for (int i = 0; i < N; ++i) {
                    double v = 0;
                    for (int k = 1; k <= hm; ++k)
                        if (h[k] != 0) v += h[k] * sn[size_t(k) * size_t(i) & (N - 1)];
                    tmp[i] = v;
                    pk = std::max(pk, std::fabs(v));
                }
                float* out = &d[(size_t(f) * LV + l) * N];
                const double g = pk > 1e-9 ? 1.0 / pk : 0.0;
                for (int i = 0; i < N; ++i) out[i] = float(tmp[i] * g);
            }
        }
    }

    inline float read(int f, int l, double ph) const {
        const float* t = &d[(size_t(f) * LV + l) * N];
        const double x = ph * N;
        int i0 = int(x);
        const double fr = x - i0;
        i0 &= N - 1;
        const int i1 = (i0 + 1) & (N - 1);
        return float(t[i0] + (t[i1] - t[i0]) * fr);
    }
    inline float sample(double pos, double ph, int l) const {
        const double x = pos * (FR - 1);
        int f0 = int(x);
        if (f0 >= FR - 1) f0 = FR - 2;
        const double ff = x - f0;
        const float a = read(f0, l, ph), b = read(f0 + 1, l, ph);
        return float(a + (b - a) * ff);
    }
    static int level(double freq, double sr) {
        const double hmax = 0.45 * sr / std::max(1.0, freq);
        int l = 0;
        while (l < LV - 1 && (KMAX >> l) > hmax) ++l;
        return l;
    }
};

inline const WaveBank& bank() {
    static const WaveBank b = [] { WaveBank w; w.build(); return w; }();
    return b;
}

// Enveloppe : attaque linéaire, decay/release exponentiels (comme la maquette)
struct Env {
    enum St { Idle, Att, Dec, Rel } st = Idle;
    double v = 0, aInc = 1, dCoef = 0, rCoef = 0, sus = 0;
    void start(double A, double D, double S, double sr) {
        st = Att;
        aInc = 1.0 / std::max(1.0, A * sr);
        dCoef = std::exp(-1.0 / (std::max(1e-4, D / 3) * sr));
        sus = S;
    }
    void release(double R, double sr) {
        st = Rel;
        rCoef = std::exp(-1.0 / (std::max(1e-4, R / 3) * sr));
    }
    inline double next() {
        switch (st) {
            case Att: v += aInc; if (v >= 1) { v = 1; st = Dec; } break;
            case Dec: v = sus + (v - sus) * dCoef; if (std::fabs(v - sus) < 1e-6) v = sus; break;
            case Rel: v *= rCoef; if (v < 1e-5) { v = 0; st = Idle; } break;
            default: break;
        }
        return v;
    }
    bool idle() const { return st == Idle; }
};

// Filtre passe-bas résonant (SVF topologie TPT)
struct SVF {
    double ic1 = 0, ic2 = 0;
    inline double lp(double x, double g, double k) {
        const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return v2;
    }
};

// Jeu de la vie (36 x 16), même logique que la maquette
struct Life {
    static constexpr int W = 36, H = 16;
    uint8_t c[W * H] = {};
    uint32_t rng = 0x9E3779B9u;
    double dens = 0;
    uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
    double rf() { return (rnd() & 0xFFFFFF) / 16777216.0; }
    void clear() { std::memset(c, 0, sizeof(c)); dens = 0; }
    void seed(int n) { for (int i = 0; i < n; ++i) c[rnd() % (W * H)] = 1; count(); }
    void spawn(int key, double vel, double amt) {
        const int col = std::clamp(int(std::lround((key - 36) / 48.0 * (W - 1))), 0, W - 1);
        const int cnt = int(std::lround((3 + vel * 9) * (.4 + amt * 1.2)));
        const int r0 = int(rnd() % H);
        for (int i = 0; i < cnt; ++i) {
            const int x = (col + int(rnd() % 5) - 2 + W) % W;
            const int y = (r0 + int(rnd() % 5) - 2 + H) % H;
            c[y * W + x] = 1;
        }
    }
    void step(int rule) {
        static constexpr uint16_t BORN[3] = {1 << 3, (1 << 3) | (1 << 6), 1 << 2};
        static constexpr uint16_t SURV[3] = {(1 << 2) | (1 << 3), (1 << 2) | (1 << 3), 0};
        rule = std::clamp(rule, 0, 2);
        uint8_t nx[W * H];
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if ((dx || dy) && c[((y + dy + H) % H) * W + (x + dx + W) % W]) ++n;
                const int i = y * W + x;
                const uint8_t a = c[i];
                if (a) nx[i] = ((SURV[rule] >> n) & 1) ? uint8_t(std::min(a + 1, 9)) : 0;
                else nx[i] = ((BORN[rule] >> n) & 1) ? 1 : 0;
            }
        std::memcpy(c, nx, sizeof(c));
        count();
    }
    void count() { int a = 0; for (uint8_t v : c) a += v ? 1 : 0; dens = a / double(W * H); }
};

struct NoteEnd { int nid, ch, key; uint32_t time; };
struct NoteVis { int key; float vel; };

struct Snap {
    double o[2][OSC_NP] = {};
    double master = .6, lifeAmt = .5, lifeSpd = .5, lifeSpawn = .5;
    int mix = 0, tgt = 2, rule = 0;
    double fx[kNumFx][5] = {}; // [effet][0 = marche/arrêt, 1..4 = réglages]
};

// Identifiant réservé aux notes jouées sur le clavier de l'interface (jamais signalées au DAW)
constexpr int kUiNoteId = -2;

class Engine {
public:
    static constexpr int NV = 16, MAXU = 5;
    static constexpr double kBus = 0.8;   // gain de bus avant le limiteur
    static constexpr double kThr = 0.89;  // seuil du limiteur (-1 dB)

    struct Chain { double ph[MAXU] = {}, inc[MAXU] = {}; int lv = 0; SVF f; Env e; };
    struct NoteReq { int key = 0, ch = 0, nid = -1; double vel = 0; };
    struct Voice {
        bool on = false, rel = false, pend = false, pendRel = false;
        int key = 0, ch = 0, nid = -1, mode = 0;
        double pk = 0, fade = 1;
        uint64_t stamp = 0;
        NoteReq pn;
        Chain c[2];
    };

    Voice v[NV];
    NoteEnd ends[64];
    int nEnds = 0;
    Life life;
    bool frozen = false;
    bool limiterOn = true;
    uint32_t now = 0; // position (en échantillons) dans le bloc en cours

    // Données lues par l'interface (thread principal)
    Spsc<NoteVis, 256> visNotes;
    std::atomic<uint8_t> lifeView[Life::W * Life::H];
    std::atomic<float> densView{0}, gainView{1};

    Engine() { for (auto& a : lifeView) a.store(0, std::memory_order_relaxed); }

    FxRack fx;

    void init(double s) {
        sr = s;
        fx.init(s);
        for (auto& x : v) x = Voice{};
        nEnds = 0;
        life.clear();
        life.seed(70);
        publishLife();
        lifeSig = lifeSm = lifeDet = 0;
        lifeCount = 0;
        ctl = 0;
        first = true;
        limG = 1;
        aPar = 1 - std::exp(-1 / (0.02 * sr));
        aLife = 1 - std::exp(-1 / (0.04 * sr));
        laN = std::clamp(int(std::lround(0.0015 * sr)), 8, kLaMax);
        for (int k = 0; k < kLaMax; ++k) { laL[k] = laR[k] = 0; laT[k] = 1; }
        laPos = 0;
        limAtk = 1 - std::exp(-5.0 / laN); // rattrape la cible en moins de laN échantillons
        limRel = 1 - std::exp(-1 / (0.12 * sr));
        fadeStep = 1 / (0.003 * sr);
    }

    void reset() {
        for (auto& x : v) {
            if (!x.on) continue;
            if (x.pend) pushEndReq(x.pn); else pushEnd(x);
            x = Voice{};
        }
    }

    bool anyActive() const { for (auto& x : v) if (x.on) return true; return false; }

    void noteOn(int key, int ch, int nid, double vel, const Snap& s) {
        if (key < 0 || key > 127) return;
        control(s);
        visNotes.push({key, float(vel)});
        life.spawn(key, vel, s.lifeSpawn);
        const NoteReq rq{key, ch, nid, vel};
        // Même note rejouée : on garde la voix (pas de clic)
        for (auto& x : v)
            if (x.on && !x.pend && x.key == key && x.ch == ch) { pushEnd(x); begin(x, rq, s, false); return; }
        for (auto& x : v)
            if (!x.on) { begin(x, rq, s, true); return; }
        // Vol de voix : fondu de 3 ms avant réutilisation
        Voice* t = nullptr;
        for (auto& x : v) {
            if (!t) { t = &x; continue; }
            const int sx = x.pend ? 2 : (x.rel ? 0 : 1), st0 = t->pend ? 2 : (t->rel ? 0 : 1);
            if (sx < st0 || (sx == st0 && x.stamp < t->stamp)) t = &x;
        }
        if (t->pend) pushEndReq(t->pn);
        else { pushEnd(*t); t->pend = true; }
        t->pn = rq;
        t->pendRel = false;
        t->stamp = ++counter;
    }

    void noteOff(int key, int ch, int nid, const Snap& s) {
        for (auto& x : v) {
            if (!x.on) continue;
            if (x.pend) { if (match(x.pn.key, x.pn.ch, x.pn.nid, key, ch, nid)) x.pendRel = true; continue; }
            if (!x.rel && match(x.key, x.ch, x.nid, key, ch, nid)) release(x, s);
        }
    }

    void choke(int key, int ch, int nid) {
        for (auto& x : v) {
            if (!x.on) continue;
            if (x.pend) { if (match(x.pn.key, x.pn.ch, x.pn.nid, key, ch, nid)) { pushEndReq(x.pn); x = Voice{}; } continue; }
            if (match(x.key, x.ch, x.nid, key, ch, nid)) { pushEnd(x); x = Voice{}; }
        }
    }

    void clearLife() { life.clear(); lifeSig = 0; publishLife(); }

    void render(float* L, float* R, uint32_t n, const Snap& s, uint32_t offset = 0) {
        const WaveBank& wb = bank();
        for (uint32_t i = 0; i < n; ++i) {
            now = offset + i;
            if ((ctl++ & 15) == 0) control(s);
            for (int X = 0; X < 2; ++X) {
                lvlC[X] += (lvlS[X] - lvlC[X]) * aPar;
                panLC[X] += (panL[X] - panLC[X]) * aPar;
                panRC[X] += (panR[X] - panRC[X]) * aPar;
                lfoPh[X] += lfoInc[X];
                if (lfoPh[X] >= 1) lfoPh[X] -= 1;
                lfoV[X] = std::sin(2 * PI * lfoPh[X]);
            }
            mst += (s.master - mst) * aPar;
            if ((lifeCount += 1) >= lifeInterval) {
                lifeCount = 0;
                if (!frozen) life.step(s.rule);
                lifeSig = std::min(1.0, life.dens * 5);
                publishLife();
            }
            lifeSm += (lifeSig - lifeSm) * aLife;

            double l = 0, r = 0;
            for (auto& x : v) {
                if (!x.on) continue;
                if (x.pend) {
                    x.fade -= fadeStep;
                    if (x.fade <= 0) {
                        const bool relNow = x.pendRel; // begin() remet pendRel à zéro
                        begin(x, x.pn, s, true);
                        if (relNow) release(x, s);
                    }
                }
                Chain& A = x.c[0];
                Chain& B = x.c[1];
                double sB = 0;
                for (int k = 0; k < U[1]; ++k) {
                    sB += wb.sample(effPos[1], B.ph[k], B.lv);
                    B.ph[k] += B.inc[k];
                    B.ph[k] -= std::floor(B.ph[k]);
                }
                sB *= lvlC[1];
                const double fm = (x.mode == 1) ? 1 + 3 * sB : 1;
                double sA = 0;
                for (int k = 0; k < U[0]; ++k) {
                    sA += wb.sample(effPos[0], A.ph[k], A.lv);
                    A.ph[k] += A.inc[k] * fm;
                    A.ph[k] -= std::floor(A.ph[k]);
                }
                sA *= lvlC[0];
                const double g = x.pk * x.fade;
                const double oA = A.f.lp(sA, gF[0], kF[0]) * A.e.next() * g;
                const double AL = oA * panLC[0], AR = oA * panRC[0];
                if (x.mode == 1) { l += AL; r += AR; }
                else {
                    const double oB = B.f.lp(sB, gF[1], kF[1]) * B.e.next() * x.pk;
                    if (x.mode == 0) { l += AL + oB * x.fade * panLC[1]; r += AR + oB * x.fade * panRC[1]; }
                    else { const double m = oB * (panLC[1] + panRC[1]) * .5 * 4; l += AL * m; r += AR * m; }
                }
                if (!x.pend && A.e.idle() && (x.mode == 1 || B.e.idle())) { pushEnd(x); x.on = false; }
            }
            fx.process(l, r);
            l *= kBus * mst;
            r *= kBus * mst;
            if (limiterOn) {
                // Limiteur à anticipation : le gain baisse AVANT que la crête n'arrive
                const double pk = std::max(std::fabs(l), std::fabs(r));
                const double dl = laL[laPos], dr = laR[laPos]; // échantillon entré il y a laN échantillons
                laL[laPos] = l; laR[laPos] = r;
                laT[laPos] = pk > kThr ? kThr / pk : 1.0;
                laPos = (laPos + 1) % laN;
                double tg = 1.0;
                for (int k = 0; k < laN; ++k) tg = std::min(tg, laT[k]);
                limG += (tg - limG) * (tg < limG ? limAtk : limRel);
                l = dl * limG;
                r = dr * limG;
            }
            L[i] = float(std::clamp(l, -1.0, 1.0));
            R[i] = float(std::clamp(r, -1.0, 1.0));
        }
        gainView.store(float(limG), std::memory_order_relaxed);
    }

    // Latence ajoutée par le limiteur (à déclarer au DAW)
    uint32_t latency() const { return limiterOn ? uint32_t(laN) : 0; }
    double lifeSignal() const { return lifeSig; }
    double lifeSmoothed() const { return lifeSm; }
    double effectivePos(int X) const { return effPos[X]; }
    double masterSmoothed() const { return mst; }

private:
    double sr = 48000;
    uint64_t counter = 0;
    uint32_t ctl = 0;
    bool first = true;
    double aPar = .001, aLife = .001, limAtk = .04, limRel = .0002, fadeStep = .007, limG = 1, mst = .6;
    double lfoPh[2] = {}, lfoV[2] = {}, lfoInc[2] = {}, effPos[2] = {};
    double lvlS[2] = {1, 1}, lvlC[2] = {1, 1};
    double panL[2] = {.7071, .7071}, panR[2] = {.7071, .7071}, panLC[2] = {.7071, .7071}, panRC[2] = {.7071, .7071};
    double gF[2] = {.5, .5}, kF[2] = {1, 1}, semi[2] = {}, detC[2] = {};
    int U[2] = {1, 1};
    static constexpr int kLaMax = 512;
    int laN = 72, laPos = 0;
    double laL[kLaMax] = {}, laR[kLaMax] = {}, laT[kLaMax] = {};
    double lifeSig = 0, lifeSm = 0, lifeDet = 0, lifeCount = 0, lifeInterval = 1;

    static bool match(int vk, int vc, int vn, int key, int ch, int nid) {
        return (key < 0 || vk == key) && (ch < 0 || vc == ch) && (nid < 0 || vn == nid);
    }
    void pushEndReq(const NoteReq& q) {
        if (q.nid == kUiNoteId || nEnds >= 64) return;
        ends[nEnds++] = {q.nid, q.ch, q.key, now};
    }
    void pushEnd(const Voice& x) { pushEndReq({x.key, x.ch, x.nid, 0}); }

    void publishLife() {
        for (int i = 0; i < Life::W * Life::H; ++i) lifeView[i].store(life.c[i], std::memory_order_relaxed);
        densView.store(float(life.dens), std::memory_order_relaxed);
    }

    void begin(Voice& x, const NoteReq& q, const Snap& s, bool fresh) {
        x.on = true; x.rel = false; x.pend = false; x.pendRel = false; x.fade = 1;
        x.key = q.key; x.ch = q.ch; x.nid = q.nid;
        x.mode = s.mix; x.pk = .2 + .25 * q.vel; x.stamp = ++counter;
        for (int X = 0; X < 2; ++X) {
            Chain& c = x.c[X];
            if (fresh) {
                c.f = SVF{};
                c.e = Env{};
                for (int k = 0; k < MAXU; ++k) c.ph[k] = k ? life.rf() : 0;
            }
            const double* o = s.o[X];
            c.e.start(tm(o[ATK]), tm(o[DEC]), o[SUS], sr);
        }
        freqs(x);
    }

    void release(Voice& x, const Snap& s) {
        x.rel = true;
        for (int X = 0; X < 2; ++X) x.c[X].e.release(tm(s.o[X][REL]), sr);
    }

    void freqs(Voice& x) {
        for (int X = 0; X < 2; ++X) {
            const double f = 440 * std::pow(2.0, (x.key - 69 + semi[X]) / 12.0);
            const int u = U[X];
            for (int k = 0; k < u; ++k) {
                const double c = u > 1 ? (double(k) / (u - 1) * 2 - 1) * detC[X] : 0;
                x.c[X].inc[k] = f * std::pow(2.0, c / 1200) / sr;
            }
            const double top = f * std::pow(2.0, detC[X] / 1200) * ((x.mode == 1 && X == 0) ? 4 : 1);
            x.c[X].lv = WaveBank::level(top, sr);
        }
    }

    void control(const Snap& s) {
        for (int X = 0; X < 2; ++X) {
            const double* o = s.o[X];
            const bool hit = (s.tgt == 1 && X == 0) || (s.tgt == 2 && X == 1);
            effPos[X] = std::clamp(o[POS] + (hit ? lifeSm * s.lifeAmt * .6 : 0), 0.0, 1.0);
            U[X] = 1 + int(std::lround(o[UNI] * 4));
            lvlS[X] = o[LVL] / std::sqrt(double(U[X]));
            const double px = ((o[PAN] - .5) * 2 + 1) / 2;
            panL[X] = std::cos(px * PI / 2);
            panR[X] = std::sin(px * PI / 2);
            lfoInc[X] = lr(o[LFOR]) / sr;
            const double cents = lfoV[X] * 2400 * o[LFOD] + lifeDet;
            const double fc = std::clamp(hz(o[CUT]) * std::pow(2.0, cents / 1200), 20.0, .45 * sr);
            gF[X] = std::tan(PI * fc / sr);
            kF[X] = 1.0 / qq(o[RES]);
            semi[X] = st(o[SEMI]);
            detC[X] = o[DET] * 60;
            if (first) { lvlC[X] = lvlS[X]; panLC[X] = panL[X]; panRC[X] = panR[X]; }
        }
        if (first) { mst = s.master; first = false; }
        const double target = s.tgt == 3 ? lifeSm * s.lifeAmt * 2400 : 0;
        lifeDet += (target - lifeDet) * (1 - std::exp(-16.0 / (.08 * sr)));
        lifeInterval = std::max(1.0, spd(s.lifeSpd) / 1000 * sr);
        fx.control(s.fx);
        for (auto& x : v) if (x.on && !x.pend) freqs(x);
    }
};

} // namespace raw
