/*
  SpectralMath.h — výpočty ze spektra AS7265x (bez závislosti na Arduinu,
  takže jdou otestovat i na PC: viz test/test_spectral_math.cpp).

  Vstup: 18 kanálů 410–940 nm, nezáporné, všechny se stejným gainem
  a integračním časem (měřítko se v poměrech vykrátí).

  * 2026 chiptron.cz
*/
#pragma once

#include <math.h>
#include <stdint.h>

namespace spectral {

constexpr int CHANNELS = 18;
constexpr int VIS_LAST = 13;   // 410–760 nm (index 0..13), "viditelná" část
constexpr int NIR_FIRST = 14;  // 810–940 nm (index 14..17)
constexpr int XYZ_CHANNELS = 14;

// Nominální středy kanálů. Pořadí odpovídá readSensor() v .ino.
constexpr int WAVELENGTHS[CHANNELS] = {
    410, 435, 460, 485, 510, 535,   // A B C D E F (vše AS72653)
    560, 585, 610, 645, 680, 705,   // G H R I S J (G H I J = AS72652, R S = AS72651)
    730, 760, 810, 860, 900, 940    // T U V W K L (T U V W = AS72651, K L = AS72652)
};

// Šířka intervalu, který kanál zastupuje (hranice v polovině mezi sousedními
// středy). Kanály nejsou rozmístěné rovnoměrně (25–50 nm), takže bez těchto
// vah by se širší intervaly v červené a NIR podhodnotily.
constexpr float BIN_WIDTH[CHANNELS] = {
    25.0f, 25.0f, 25.0f, 25.0f, 25.0f, 25.0f,
    25.0f, 25.0f, 30.0f, 35.0f, 30.0f, 25.0f,
    27.5f, 40.0f, 50.0f, 45.0f, 40.0f, 40.0f
};

// Odhad CIE 1931 XYZ (2°) z kanálů 410–760 nm.
// Váhy jsou fitované metodou nejmenších čtverců na modelu senzoru
// (gaussovské kanály, FWHM 20 nm) a sadě spekter: Planck, denní světlo D,
// bílé/teplé LED, výbojky. Postup: tools/compute_weights.py
// Ověřeno na iluminantech CIE mimo trénovací sadu (LED-B1..B5, HP1..HP5,
// FL...): chyba CCT typicky < 1 %, u čárových spekter až ~10 %.
constexpr float W_X[XYZ_CHANNELS] = {-0.01587f, +0.41310f, +0.32864f, +0.07655f, +0.09456f, +0.15060f, +0.75058f, +1.10410f, +1.44736f, +0.58662f, +0.13115f, -0.01867f, -0.03878f, +0.01565f};
constexpr float W_Y[XYZ_CHANNELS] = {-0.01781f, +0.01842f, +0.05471f, +0.19095f, +0.65482f, +1.01871f, +1.24589f, +0.90102f, +0.70939f, +0.22068f, +0.03701f, -0.02066f, +0.00402f, +0.00395f};
constexpr float W_Z[XYZ_CHANNELS] = {+0.20946f, +1.93809f, +1.97424f, +0.64433f, +0.17901f, +0.05170f, +0.04284f, -0.01299f, -0.00197f, +0.00279f, +0.05553f, -0.09377f, +0.04395f, -0.00603f};

// CCT má podle CIE smysl jen blízko Planckovy křivky.
constexpr float DUV_LIMIT = 0.05f;
constexpr double CCT_MIN = 1000.0;
constexpr double CCT_MAX = 15000.0;

struct Metrics
{
    float blueRatio;   // 0..1, podíl 410–485 nm (interval 397,5–497,5 nm) ve 410–760 nm
    float coldIndex;   // modrá (397,5–497,5 nm) / červená (597,5–717,5 nm)
    float nirRatio;    // 0..1, podíl 810–940 nm v celém rozsahu
    float X, Y, Z;     // odhad CIE XYZ (Y ve stejných jednotkách jako vstup)
    float x, y;        // chromatičnost CIE 1931
    float cct;         // K, NAN pokud nelze určit
    float duv;         // vzdálenost od Planckovy křivky v CIE 1960 uv, NAN pokud nelze
    bool cctValid;     // cct v rozsahu a |duv| <= DUV_LIMIT
};

inline float bandSum(const float *s, int first, int last)
{
    float sum = 0;
    for (int i = first; i <= last; i++) sum += s[i] * BIN_WIDTH[i];
    return sum;
}

inline float safeRatio(float num, float den)
{
    return (den > 1e-9f) ? num / den : NAN;
}

// Planckova křivka v CIE 1960 uv, aproximace Krystek (1985), 1000–15000 K.
inline void planckUV(double T, double &u, double &v)
{
    const double T2 = T * T;
    u = (0.860117757 + 1.54118254e-4 * T + 1.28641212e-7 * T2) /
        (1.0 + 8.42420235e-4 * T + 7.08145163e-7 * T2);
    v = (0.317398726 + 4.22806245e-5 * T + 4.20481691e-8 * T2) /
        (1.0 - 2.89741816e-5 * T + 1.61456053e-7 * T2);
}

inline double locusDist2(double mired, double u, double v)
{
    double pu, pv;
    planckUV(1e6 / mired, pu, pv);
    return (u - pu) * (u - pu) + (v - pv) * (v - pv);
}

// CCT a Duv hledáním nejbližšího bodu na Planckově křivce.
// Nahrazuje McCamyho vzorec (ten Duv neumí a mimo ~2000–10000 K rychle ztrácí přesnost).
// Vrací false, pokud je bod mimo rozsah nebo příliš daleko od křivky.
inline bool cctDuv(double x, double y, float &cctOut, float &duvOut)
{
    cctOut = NAN;
    duvOut = NAN;

    const double d = -2.0 * x + 12.0 * y + 3.0;
    if (!(d > 1e-9)) return false;
    const double u = 4.0 * x / d;
    const double v = 6.0 * y / d;

    // 1) hrubé hledání v mired (rovnoměrnější než v kelvinech)
    const double mMin = 1e6 / CCT_MAX, mMax = 1e6 / CCT_MIN, step = 2.0;
    double bestM = mMin, bestD = 1e30;
    for (double m = mMin; m <= mMax; m += step)
    {
        const double dist = locusDist2(m, u, v);
        if (dist < bestD) { bestD = dist; bestM = m; }
    }

    // 2) zpřesnění zlatým řezem
    double a = bestM - step, b = bestM + step;
    if (a < mMin) a = mMin;
    if (b > mMax) b = mMax;
    const double g = 0.6180339887498949;
    double c1 = b - g * (b - a), c2 = a + g * (b - a);
    double f1 = locusDist2(c1, u, v), f2 = locusDist2(c2, u, v);
    for (int i = 0; i < 40; i++)
    {
        if (f1 < f2) { b = c2; c2 = c1; f2 = f1; c1 = b - g * (b - a); f1 = locusDist2(c1, u, v); }
        else         { a = c1; c1 = c2; f1 = f2; c2 = a + g * (b - a); f2 = locusDist2(c2, u, v); }
    }
    const double m = 0.5 * (a + b);
    const double T = 1e6 / m;

    double pu, pv;
    planckUV(T, pu, pv);
    const double dist = sqrt((u - pu) * (u - pu) + (v - pv) * (v - pv));
    const double duv = (v >= pv) ? dist : -dist;  // kladné = nad křivkou (do zelena)

    cctOut = (float)T;
    duvOut = (float)duv;

    // Minimum na kraji rozsahu = skutečná CCT je mimo 1000–15000 K
    const bool atEdge = (m <= mMin + step) || (m >= mMax - step);
    return !atEdge && fabs(duv) <= DUV_LIMIT;
}

inline Metrics compute(const float *s)
{
    Metrics r;

    const float blue    = bandSum(s, 0, 3);          // 410–485 nm
    const float red     = bandSum(s, 8, 11);         // 610–705 nm
    const float visible = bandSum(s, 0, VIS_LAST);   // 410–760 nm
    const float nir     = bandSum(s, NIR_FIRST, CHANNELS - 1);

    r.blueRatio = safeRatio(blue, visible);
    r.coldIndex = safeRatio(blue, red);
    r.nirRatio  = safeRatio(nir, visible + nir);

    r.X = r.Y = r.Z = 0;
    for (int i = 0; i < XYZ_CHANNELS; i++)
    {
        r.X += W_X[i] * s[i];
        r.Y += W_Y[i] * s[i];
        r.Z += W_Z[i] * s[i];
    }

    const float sum = r.X + r.Y + r.Z;
    r.cctValid = false;
    r.cct = r.duv = r.x = r.y = NAN;
    if (sum > 1e-9f && r.Y > 0)
    {
        r.x = r.X / sum;
        r.y = r.Y / sum;
        r.cctValid = cctDuv(r.x, r.y, r.cct, r.duv);
    }
    return r;
}

}  // namespace spectral
