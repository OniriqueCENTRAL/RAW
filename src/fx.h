// RAW - les 10 effets, dans l'ordre de la chaîne (même réglages que la maquette web)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "params.h"

namespace raw {

// Ligne à retard avec lecture fractionnaire
struct DelayLine {
    std::vector<float> b;
    int w = 0, mask = 0;
    void init(double seconds, double sr) {
        int n = 1;
        while (n < int(seconds * sr) + 4) n <<= 1;
        b.assign(size_t(n), 0.f);
        mask = n - 1;
        w = 0;
    }
    void clear() { std::fill(b.begin(), b.end(), 0.f); }
    inline void push(double x) { b[size_t(w)] = float(x); w = (w + 1) & mask; }
    // d = retard en échantillons (1 = le plus récent)
    inline double read(double d) const {
        d = std::clamp(d, 1.0, double(mask - 2));
        double rp = double(w) - d;
        if (rp < 0) rp += double(mask + 1);
        const int i0 = int(rp);
        const double f = rp - i0;
        const double a = b[size_t(i0 & mask)], c = b[size_t((i0 + 1) & mask)];
        return a + (c - a) * f;
    }
};

// Filtre biquad (formules RBJ), forme transposée II
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void clear() { z1 = z2 = 0; }
    inline double tick(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void set(double B0, double B1, double B2, double A0, double A1, double A2) {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void allpass(double f, double q, double sr) {
        const double w = 2 * 3.14159265358979 * f / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(1 - al, -2 * c, 1 + al, 1 + al, -2 * c, 1 - al);
    }
    void peaking(double f, double q, double db, double sr) {
        const double A = std::pow(10.0, db / 40), w = 2 * 3.14159265358979 * f / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void lowShelf(double f, double db, double sr) {
        const double A = std::pow(10.0, db / 40), w = 2 * 3.14159265358979 * f / sr, c = std::cos(w);
        const double al = std::sin(w) / 2 * std::sqrt(2.0), s = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
            (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }
    void highShelf(double f, double db, double sr) {
        const double A = std::pow(10.0, db / 40), w = 2 * 3.14159265358979 * f / sr, c = std::cos(w);
        const double al = std::sin(w) / 2 * std::sqrt(2.0), s = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
            (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }
};

// Filtre à variables d'état multimode (passe-bas, passe-bande, passe-haut)
struct SvfMulti {
    double ic1 = 0, ic2 = 0;
    void clear() { ic1 = ic2 = 0; }
    inline double tick(double x, double g, double k, int type) {
        const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return type == 0 ? v2 : type == 1 ? v1 : x - k * v1 - v2;
    }
};

// Réverbération algorithmique (structure Freeverb : 8 filtres en peigne + 4 passe-tout par côté)
struct Comb {
    std::vector<float> b; int i = 0; double fs = 0;
    void clear() { std::fill(b.begin(), b.end(), 0.f); fs = 0; }
    inline double tick(double x, double fb, double d) {
        const double y = b[size_t(i)];
        fs = y * (1 - d) + fs * d;
        b[size_t(i)] = float(x + fs * fb);
        if (++i >= int(b.size())) i = 0;
        return y;
    }
};
struct AllpassR {
    std::vector<float> b; int i = 0;
    void clear() { std::fill(b.begin(), b.end(), 0.f); }
    inline double tick(double x) {
        const double bo = b[size_t(i)], y = -x + bo;
        b[size_t(i)] = float(x + bo * .5);
        if (++i >= int(b.size())) i = 0;
        return y;
    }
};

class FxRack {
public:
    enum { HYP, DST, FLA, PHA, CHO, DLY, CMP, EQQ, FLT, REV };

    void init(double s) {
        sr = s;
        aG = 1 - std::exp(-1 / (0.02 * sr));
        aDly = 1 - std::exp(-1 / (0.08 * sr));
        cAtt = std::exp(-1 / (0.005 * sr));
        cRel = std::exp(-1 / (0.15 * sr));
        hypLine.init(.06, sr);
        choLine.init(.06, sr);
        for (int c = 0; c < 2; ++c) { flaLine[c].init(.02, sr); dlyLine[c].init(1.1, sr); }
        static const int combT[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int apT[4] = {556, 441, 341, 225};
        const double k = sr / 44100.0;
        double sum = 0;
        for (int c = 0; c < 2; ++c) {
            for (int j = 0; j < 8; ++j) { comb[c][j].b.assign(size_t((combT[j] + c * 23) * k) + 1, 0.f); comb[c][j].i = 0; }
            for (int j = 0; j < 4; ++j) { ap[c][j].b.assign(size_t((apT[j] + c * 23) * k) + 1, 0.f); ap[c][j].i = 0; }
        }
        for (int j = 0; j < 8; ++j) sum += combT[j];
        combAvg = sum / 8 / 44100.0;
        for (auto& x : sl) x = Slot{};
        dlySm = 0.4 * sr;
        dlyLpA = 1 - std::exp(-2 * 3.14159265358979 * 4500 / sr); // passe-bas dans la boucle du delay
    }

    // Appelé tous les 16 échantillons : recalcule les coefficients
    void control(const double fx[kNumFx][5]) {
        for (int i = 0; i < kNumFx; ++i) sl[i].on = fx[i][0] > .5;
        const double* p;
        p = fx[HYP]; sl[HYP].mix = p[3];
        for (int k = 0; k < 3; ++k) hypInc[k] = (.1 + p[1] * 3) * (1 + k * .37) / sr;
        hypDepth = (.0008 + p[2] * .006) * sr;
        p = fx[DST]; sl[DST].mix = p[3]; dstK = 1 + p[1] * 40; dstG = tanG(600 * std::pow(25.0, p[2]));
        p = fx[FLA]; sl[FLA].mix = p[4]; flaInc = (.05 + p[1] * 4) / sr; flaDepth = (.0005 + p[2] * .003) * sr; flaFb = p[3] * .85;
        p = fx[PHA]; sl[PHA].mix = p[3]; phaInc = (.05 + p[1] * 3) / sr; phaDepth = p[2] * 2000;
        if (sl[PHA].on || sl[PHA].running) {
            const double cents = phaDepth * std::sin(2 * 3.14159265358979 * phaPh);
            for (int k = 0; k < 4; ++k) {
                const double f = std::min(.45 * sr, 700 * (1 + k * .6) * std::pow(2.0, cents / 1200));
                for (int c = 0; c < 2; ++c) phaAp[c][k].allpass(f, .7, sr);
            }
        }
        p = fx[CHO]; sl[CHO].mix = p[3];
        for (int k = 0; k < 2; ++k) choInc[k] = (.1 + p[1] * 2.5) * (1 + k * .3) / sr;
        choDepth = (.001 + p[2] * .005) * sr;
        p = fx[DLY]; sl[DLY].mix = p[3]; dlyT = (.03 + p[1] * .97) * sr; dlyFb = p[2] * .85;
        p = fx[CMP]; sl[CMP].mix = 1; cmpT = -60 + p[1] * 60; cmpR = 1 + p[2] * 19;
        // gain automatique (comme le compresseur de la maquette) + gain réglé par l'utilisateur
        cmpMk = -cmpT * (1 - 1 / cmpR) * .5 + p[3] * 12;
        p = fx[EQQ]; sl[EQQ].mix = 1;
        if (sl[EQQ].on || sl[EQQ].running) {
            for (int c = 0; c < 2; ++c) {
                eq[c][0].lowShelf(200, (p[1] - .5) * 24, sr);
                eq[c][1].peaking(1000, .8, (p[2] - .5) * 24, sr);
                eq[c][2].highShelf(5000, (p[3] - .5) * 24, sr);
            }
        }
        p = fx[FLT]; sl[FLT].mix = 1; fltG = tanG(hz(p[1])); fltK = 1 / qq(p[2]); fltType = std::clamp(int(std::lround(p[3])), 0, 2);
        p = fx[REV]; sl[REV].mix = p[3];
        revFb = std::pow(10.0, -3 * combAvg / (.4 + p[1] * 3.6)); // temps de déclin = durée affichée
        revD = std::exp(-2 * 3.14159265358979 * (16000 - p[2] * 14500) / sr);
    }

    inline void process(double& l, double& r) {
        double wl, wr;
        if (gate(HYP)) { hyper(l, r, wl, wr); blend(HYP, l, r, wl, wr); }
        if (gate(DST)) {
            wl = dstTone[0].tick(std::tanh(dstK * l), dstG, 1, 0) * .5;
            wr = dstTone[1].tick(std::tanh(dstK * r), dstG, 1, 0) * .5;
            blend(DST, l, r, wl, wr);
        }
        if (gate(FLA)) {
            flaPh = wrap(flaPh + flaInc);
            const double d = .004 * sr + flaDepth * std::sin(2 * 3.14159265358979 * flaPh);
            wl = flaLine[0].read(d); flaLine[0].push(l + wl * flaFb);
            wr = flaLine[1].read(d); flaLine[1].push(r + wr * flaFb);
            blend(FLA, l, r, wl, wr);
        }
        if (gate(PHA)) {
            phaPh = wrap(phaPh + phaInc);
            wl = l; wr = r;
            for (int k = 0; k < 4; ++k) { wl = phaAp[0][k].tick(wl); wr = phaAp[1][k].tick(wr); }
            blend(PHA, l, r, wl, wr);
        }
        if (gate(CHO)) { chorus(l, r, wl, wr); blend(CHO, l, r, wl, wr); }
        if (gate(DLY)) {
            dlySm += (dlyT - dlySm) * aDly;
            wl = dlyLine[0].read(dlySm); wr = dlyLine[1].read(dlySm);
            dlyLp[0] += (wl - dlyLp[0]) * dlyLpA; dlyLp[1] += (wr - dlyLp[1]) * dlyLpA;
            dlyLine[0].push(l + dlyLp[0] * dlyFb); dlyLine[1].push(r + dlyLp[1] * dlyFb);
            blend(DLY, l, r, wl, wr);
        }
        if (gate(CMP)) {
            const double pk = std::max(std::fabs(l), std::fabs(r)), x = 20 * std::log10(pk + 1e-9);
            const double W = 6, d = x - cmpT;
            double y;
            if (2 * d < -W) y = x;
            else if (2 * std::fabs(d) <= W) y = x + (1 / cmpR - 1) * (d + W / 2) * (d + W / 2) / (2 * W);
            else y = cmpT + d / cmpR;
            const double gr = y - x;
            if (cmpFresh) { cmpGr = gr; cmpFresh = false; } // démarre directement au bon niveau
            cmpGr = gr < cmpGr ? gr + (cmpGr - gr) * cAtt : gr + (cmpGr - gr) * cRel;
            const double g = std::pow(10.0, (cmpGr + cmpMk) / 20);
            wl = l * g; wr = r * g;
            blend(CMP, l, r, wl, wr);
        }
        if (gate(EQQ)) {
            wl = eq[0][2].tick(eq[0][1].tick(eq[0][0].tick(l)));
            wr = eq[1][2].tick(eq[1][1].tick(eq[1][0].tick(r)));
            blend(EQQ, l, r, wl, wr);
        }
        if (gate(FLT)) {
            wl = flt[0].tick(l, fltG, fltK, fltType);
            wr = flt[1].tick(r, fltG, fltK, fltType);
            blend(FLT, l, r, wl, wr);
        }
        if (gate(REV)) {
            const double in = (l + r) * .5 * .015;
            wl = 0; wr = 0;
            for (int j = 0; j < 8; ++j) { wl += comb[0][j].tick(in, revFb, revD); wr += comb[1][j].tick(in, revFb, revD); }
            for (int j = 0; j < 4; ++j) { wl = ap[0][j].tick(wl); wr = ap[1][j].tick(wr); }
            wl *= kRevGain; wr *= kRevGain;
            blend(REV, l, r, wl, wr);
        }
    }

    bool anyRunning() const { for (auto& x : sl) if (x.running) return true; return false; }

    static constexpr double kRevGain = 3.0;

private:
    struct Slot { bool on = false, running = false; double mix = 0, wet = 0, dry = 1; };
    double sr = 48000, aG = .001, aDly = .0003, cAtt = .99, cRel = .999;
    Slot sl[kNumFx];

    DelayLine hypLine, choLine, flaLine[2], dlyLine[2];
    double hypPh[3] = {0, .33, .66}, hypInc[3] = {}, hypDepth = 0;
    double dstK = 1, dstG = .5;
    SvfMulti dstTone[2];
    double flaPh = 0, flaInc = 0, flaDepth = 0, flaFb = 0;
    double phaPh = 0, phaInc = 0, phaDepth = 0;
    Biquad phaAp[2][4];
    double choPh[2] = {0, .5}, choInc[2] = {}, choDepth = 0;
    double dlyT = 0, dlySm = 0, dlyFb = 0, dlyLp[2] = {}, dlyLpA = .45;
    double cmpT = -30, cmpR = 4, cmpMk = 0, cmpGr = 0;
    bool cmpFresh = true;
    Biquad eq[2][3];
    SvfMulti flt[2];
    double fltG = .5, fltK = 1;
    int fltType = 0;
    Comb comb[2][8];
    AllpassR ap[2][4];
    double combAvg = .03, revFb = .8, revD = .2;

    static double wrap(double x) { return x >= 1 ? x - 1 : x; }
    double tanG(double f) const { return std::tan(3.14159265358979 * std::clamp(f, 20.0, .45 * sr) / sr); }

    // Gère marche/arrêt et dosage avec un fondu, et vide les mémoires à la remise en marche
    inline bool gate(int i) {
        Slot& s = sl[i];
        const double tw = s.on ? s.mix : 0, td = s.on ? 1 - s.mix : 1;
        s.wet += (tw - s.wet) * aG;
        s.dry += (td - s.dry) * aG;
        if (!s.on && s.wet < 1e-4) { s.running = false; s.wet = 0; s.dry = 1; return false; }
        if (!s.running) { clearFx(i); s.running = true; }
        return true;
    }
    inline void blend(int i, double& l, double& r, double wl, double wr) {
        l = l * sl[i].dry + wl * sl[i].wet;
        r = r * sl[i].dry + wr * sl[i].wet;
    }

    void clearFx(int i) {
        switch (i) {
            case HYP: hypLine.clear(); break;
            case DST: dstTone[0].clear(); dstTone[1].clear(); break;
            case FLA: flaLine[0].clear(); flaLine[1].clear(); break;
            case PHA: for (auto& c : phaAp) for (auto& b : c) b.clear(); break;
            case CHO: choLine.clear(); break;
            case DLY: dlyLine[0].clear(); dlyLine[1].clear(); dlyLp[0] = dlyLp[1] = 0; dlySm = dlyT; break;
            case CMP: cmpGr = 0; cmpFresh = true; break;
            case EQQ: for (auto& c : eq) for (auto& b : c) b.clear(); break;
            case FLT: flt[0].clear(); flt[1].clear(); break;
            case REV: for (auto& c : comb) for (auto& x : c) x.clear(); for (auto& c : ap) for (auto& x : c) x.clear(); break;
        }
    }

    inline void hyper(double l, double r, double& wl, double& wr) {
        static const double base[3] = {.011, .017, .024}, pl[3] = {.9877, .1564, .7071}, pr[3] = {.1564, .9877, .7071};
        hypLine.push((l + r) * .5);
        wl = wr = 0;
        for (int k = 0; k < 3; ++k) {
            hypPh[k] = wrap(hypPh[k] + hypInc[k]);
            const double y = hypLine.read(base[k] * sr + hypDepth * std::sin(2 * 3.14159265358979 * hypPh[k]));
            wl += y * pl[k];
            wr += y * pr[k];
        }
        wl *= .7; wr *= .7;
    }

    inline void chorus(double l, double r, double& wl, double& wr) {
        static const double base[2] = {.018, .026}, pl[2] = {.9724, .2334}, pr[2] = {.2334, .9724};
        choLine.push((l + r) * .5);
        wl = wr = 0;
        for (int k = 0; k < 2; ++k) {
            choPh[k] = wrap(choPh[k] + choInc[k]);
            const double y = choLine.read(base[k] * sr + choDepth * std::sin(2 * 3.14159265358979 * choPh[k]));
            wl += y * pl[k];
            wr += y * pr[k];
        }
    }
};

} // namespace raw
