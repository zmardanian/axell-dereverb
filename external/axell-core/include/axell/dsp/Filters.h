#pragma once
// Axell core - filters: RBJ biquads, side-chain EQ, BS.1770 K-weighting, DC blocker, 90-degree allpass pair
// JUCE-free; tested with plain g++ (tests/). Shared by the Axell plugins.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Math.h"

namespace axell
{
//==============================================================================
// RBJ biquad (high-pass / high-shelf), double precision state, up to kMaxCh channels.
class Biquad
{
public:
    void setHighPass (double fs, double freq, double q = 0.7071067811865476)
    {
        if (std::abs (freq - lastFreq) < 1.0e-9 && std::abs (fs - lastFs) < 1.0e-9 && lastType == 1)
            return;
        lastFreq = freq;
        lastFs = fs;
        lastType = 1;
        const double w0 = 2.0 * kPi * std::min (freq, fs * 0.45) / fs;
        const double cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
        b0 = (1.0 + cosw) * 0.5 / a0;
        b1 = -(1.0 + cosw) / a0;
        b2 = b0;
        a1 = -2.0 * cosw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    void setHighShelf (double fs, double freq, double gainDb)
    {
        if (std::abs (freq - lastFreq) < 1.0e-9 && std::abs (fs - lastFs) < 1.0e-9 && lastType == 2
            && std::abs (gainDb - lastGain) < 1.0e-9)
            return;
        lastFreq = freq;
        lastFs = fs;
        lastType = 2;
        lastGain = gainDb;
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * std::min (freq, fs * 0.45) / fs;
        const double cosw = std::cos (w0), sinw = std::sin (w0);
        const double alpha = sinw / 2.0 * std::sqrt (2.0); // shelf slope S = 1
        const double sq = 2.0 * std::sqrt (A) * alpha;
        const double a0 = (A + 1.0) - (A - 1.0) * cosw + sq;
        b0 = A * ((A + 1.0) + (A - 1.0) * cosw + sq) / a0;
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw) / a0;
        b2 = A * ((A + 1.0) + (A - 1.0) * cosw - sq) / a0;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw) / a0;
        a2 = ((A + 1.0) - (A - 1.0) * cosw - sq) / a0;
    }

    void setLowPass (double fs, double freq, double q = 0.7071067811865476)
    {
        if (std::abs (freq - lastFreq) < 1.0e-9 && std::abs (fs - lastFs) < 1.0e-9 && lastType == 3)
            return;
        lastFreq = freq;
        lastFs = fs;
        lastType = 3;
        const double w0 = 2.0 * kPi * std::min (freq, fs * 0.45) / fs;
        const double cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q), a0 = 1.0 + alpha;
        b0 = (1.0 - cosw) * 0.5 / a0;
        b1 = (1.0 - cosw) / a0;
        b2 = b0;
        a1 = -2.0 * cosw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    // RBJ peaking bell
    void setPeak (double fs, double freq, double q, double gainDb)
    {
        if (std::abs (freq - lastFreq) < 1.0e-9 && std::abs (fs - lastFs) < 1.0e-9 && lastType == 4
            && std::abs (gainDb - lastGain) < 1.0e-9 && std::abs (q - lastQ) < 1.0e-9)
            return;
        lastFreq = freq;
        lastFs = fs;
        lastType = 4;
        lastGain = gainDb;
        lastQ = q;
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * std::min (freq, fs * 0.45) / fs;
        const double cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * std::max (0.05, q));
        const double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = -2.0 * cosw / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = b1;
        a2 = (1.0 - alpha / A) / a0;
    }

    double process (double x, int channel) noexcept
    {
        const auto ch = (size_t) channel;
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }

    void resetState() noexcept
    {
        z1.fill (0.0);
        z2.fill (0.0);
    }

    void invalidate() noexcept { lastType = 0; }

    void setRaw (double nb0, double nb1, double nb2, double na1, double na2) noexcept
    {
        b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
        lastType = 9;
    }

private:
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    std::array<double, kMaxCh> z1 {}, z2 {};
    double lastFreq = -1.0, lastFs = -1.0, lastGain = 0.0, lastQ = 0.0;
    int lastType = 0;
};

//==============================================================================
// Side-chain EQ shared by both stages' detectors (2.0): the stage's own high-pass (1.x) plus an optional
// low-pass and bell. Off by default (LPF at 20 kHz, bell at 0 dB): then the detector is exactly 1.x's.
struct SideChainEq
{
    float lpfHz = 20000.0f, bellHz = 3000.0f, bellDb = 0.0f, bellQ = 1.0f;
    bool lpfOn() const noexcept { return lpfHz < 19999.0f; }
    bool bellOn() const noexcept { return std::abs (bellDb) > 0.01f; }
};

class SideChainFilter
{
public:
    void configure (double fs, const SideChainEq& e) noexcept
    {
        const bool l = e.lpfOn(), b = e.bellOn();
        if (l && ! useLpf)
            lpf.resetState();
        if (b && ! useBell)
            bell.resetState();
        useLpf = l;
        useBell = b;
        if (l)
            lpf.setLowPass (fs, std::clamp ((double) e.lpfHz, 200.0, fs * 0.45));
        if (b)
            bell.setPeak (fs, std::clamp ((double) e.bellHz, 20.0, fs * 0.45), std::clamp ((double) e.bellQ, 0.1, 10.0),
                          std::clamp ((double) e.bellDb, -24.0, 24.0));
    }
    double process (double x, int c) noexcept
    {
        if (useLpf)
            x = lpf.process (x, c);
        if (useBell)
            x = bell.process (x, c);
        return x;
    }
    void resetState() noexcept
    {
        lpf.resetState();
        bell.resetState();
    }
    void invalidate() noexcept
    {
        lpf.invalidate();
        bell.invalidate();
    }

private:
    Biquad lpf, bell;
    bool useLpf = false, useBell = false;
};

//==============================================================================
// ITU-R BS.1770 K-weighting (pre-filter high shelf + RLB high-pass), coefficients derived for any rate by the
// standard's bilinear forms. Used for the level-matched compare (loudness, not peaks).
class KWeighting
{
public:
    void prepare (double fs) noexcept
    {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / fs), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf.setRaw ((Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                          2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / fs), a0 = 1.0 + K / Q + K * K;
            hp.setRaw (1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
        }
        resetState();
    }
    void resetState() noexcept
    {
        shelf.resetState();
        hp.resetState();
    }
    double process (double x, int c) noexcept { return hp.process (shelf.process (x, c), c); }

private:
    Biquad shelf, hp;
};

// 5 Hz one-pole DC blocker applied only to generated even-order distortion, so the
// main signal path itself stays perfectly linear-phase (dry/wet alignment is untouched).
class DcBlocker
{
public:
    void prepare (double fs) noexcept
    {
        r = 1.0 - 2.0 * kPi * 5.0 / fs;
        resetState();
    }
    void resetState() noexcept
    {
        x1.fill (0.0);
        y1.fill (0.0);
    }
    double process (double x, int channel) noexcept
    {
        const auto ch = (size_t) channel;
        const double y = x - x1[ch] + r * y1[ch];
        x1[ch] = x;
        y1[ch] = y;
        return y;
    }

private:
    double r = 0.999;
    std::array<double, kMaxCh> x1 {}, y1 {};
};

//==============================================================================
// 90-degree phase-difference allpass pair ("IIR Hilbert transformer").
// |a + j b| is the instantaneous amplitude of the analytic signal: for a sine it is constant
// and independent of where the samples fall on the waveform. A plain sample-peak detector
// reads a different peak every half cycle, which creates non-harmonic GR modulation at high
// frequencies (that is what the 15 kHz aliasing test catches).
// Coefficients: elliptic halfband polyphase design (closed form, exact elliptic nome),
// frequency-shifted by fs/4. Sections (c - z^-2) / (1 - c z^-2); even coefficients in chain A,
// odd in chain B, chain B delayed by one sample.
class HilbertPair
{
public:
    static constexpr int kMaxCoefs = 20;

    // accurate band [fLow, fs/2 - fLow]. Error with 20 coefficients and fLow = 15 Hz:
    // 0.0012 deg at 96 kHz, 0.0136 deg at 768 kHz (192 kHz x 4).
    static void design (double fs, double fLow, int numCoefs, double* coefs)
    {
        const double t = std::clamp (2.0 * fLow / fs, 1.0e-7, 0.45);
        double k = std::tan ((1.0 - 2.0 * t) * kPi / 4.0);
        k *= k;
        auto ellipK = [] (double m)
        {
            double a = 1.0, b = std::sqrt (std::max (1.0e-300, 1.0 - m));
            for (int i = 0; i < 64 && std::abs (a - b) > 1.0e-15 * a; ++i)
            {
                const double an = 0.5 * (a + b);
                b = std::sqrt (a * b);
                a = an;
            }
            return kPi / (2.0 * a);
        };
        const double q = std::exp (-kPi * ellipK (1.0 - k * k) / ellipK (k * k));
        const int order = 2 * numCoefs + 1;
        for (int idx = 0; idx < numCoefs; ++idx)
        {
            const int c = idx + 1;
            double num = 0.0, den = 0.0;
            for (int i = 0, j = 1; i < 200; ++i, j = -j)
            {
                const double term = std::pow (q, (double) (i * (i + 1))) * std::sin ((i * 2 + 1) * c * kPi / order) * j;
                num += term;
                if (std::abs (term) < 1.0e-30)
                    break;
            }
            for (int i = 1, j = -1; i < 200; ++i, j = -j)
            {
                const double term = std::pow (q, (double) (i * i)) * std::cos (i * 2 * c * kPi / order) * j;
                den += term;
                if (std::abs (term) < 1.0e-30)
                    break;
            }
            const double ww = num * std::pow (q, 0.25) / (den + 0.5);
            const double w2 = ww * ww;
            const double x = std::sqrt ((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
            coefs[idx] = (1.0 - x) / (1.0 + x);
        }
    }

    void setCoefficients (const double* c, int n) noexcept
    {
        numCoefs = std::clamp (n, 2, kMaxCoefs);
        nA = (numCoefs + 1) / 2;
        nB = numCoefs / 2;
        for (int i = 0; i < numCoefs; ++i)
            (i % 2 == 0 ? cA[i / 2] : cB[i / 2]) = c[i];
        resetState();
    }

    void resetState() noexcept
    {
        for (auto& s : st)
            s = Chan();
    }

    // returns |analytic signal|^2
    double power (double x, int ch) noexcept
    {
        Chan& s = st[(size_t) ch];
        double a = x, b = x;
        for (int i = 0; i < nA; ++i)
        {
            const double y = cA[i] * (a + s.ay2[i]) - s.ax2[i];
            s.ax2[i] = s.ax1[i];
            s.ax1[i] = a;
            s.ay2[i] = s.ay1[i];
            s.ay1[i] = y;
            a = y;
        }
        for (int i = 0; i < nB; ++i)
        {
            const double y = cB[i] * (b + s.by2[i]) - s.bx2[i];
            s.bx2[i] = s.bx1[i];
            s.bx1[i] = b;
            s.by2[i] = s.by1[i];
            s.by1[i] = y;
            b = y;
        }
        const double bd = s.bDelay;
        s.bDelay = b;
        return a * a + bd * bd;
    }

private:
    struct Chan
    {
        double ax1[kMaxCoefs / 2] {}, ax2[kMaxCoefs / 2] {}, ay1[kMaxCoefs / 2] {}, ay2[kMaxCoefs / 2] {};
        double bx1[kMaxCoefs / 2] {}, bx2[kMaxCoefs / 2] {}, by1[kMaxCoefs / 2] {}, by2[kMaxCoefs / 2] {};
        double bDelay = 0.0;
    };
    double cA[kMaxCoefs / 2] {}, cB[kMaxCoefs / 2] {};
    int numCoefs = 2, nA = 1, nB = 1;
    std::array<Chan, kMaxCh> st {};
};

} // namespace axell
