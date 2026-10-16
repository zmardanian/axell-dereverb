#pragma once
// Axell De-Reverb - streaming rational resampler (framework independent, no JUCE).
// out rate / in rate = L / M (reduced). Polyphase FIR from a Kaiser-windowed sinc prototype designed at the
// upsampled rate L * in: cutoff 0.94 x the narrower rate's Nyquist, 24 zero crossings each side, beta 9 (stop band
// < -90 dB). Output n sits at upsampled position u = n M; y[n] = sum_k h[(u mod L) + k L] x[floor(u / L) - k].
// The network runs at 48 kHz; hosts at 44.1 / 88.2 / 96 kHz use two of these (to 48 kHz and back).
// Real-time safe after prepare(): process() never allocates.
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "DRNet.h" // dot()

namespace axdr
{
class Resampler
{
public:
    static constexpr int kZero = 24;

    void prepare (int inRate, int outRate, int maxIn)
    {
        const int g = std::gcd (inRate, outRate);
        L = outRate / g;
        M = inRate / g;
        identity = L == M;
        if (identity)
            return;
        const double fcn = 0.94 * 0.5 * std::min (1.0, (double) L / M) / L;   // cycles per upsampled sample
        const int half = (int) std::ceil (kZero / (2.0 * fcn));                  // upsampled samples each side
        taps = (2 * half + 1 + L - 1) / L;
        const int N = taps * L;
        const double c = (2 * half) / 2.0;
        auto bessel0 = [] (double x) { double s = 1, t = 1; for (int k = 1; k < 50; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double beta = 9.0, ib = 1.0 / bessel0 (beta);
        poly.assign ((size_t) N, 0.0f);
        for (int n = 0; n <= 2 * half; ++n)
        {
            const double a = 2.0 * fcn * (n - c);
            const double s = std::abs (a) < 1e-12 ? 1.0 : std::sin (kPi * a) / (kPi * a);
            const double r = (n - c) / c;
            const double w = bessel0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) * ib;
            const double h = 2.0 * fcn * s * w * L;            // x L: zero-stuffing keeps 1/L of the energy
            // polyphase layout: phase p = n mod L, tap k = n / L, stored reversed (oldest input first) so the inner
            // product runs forward through memory (vectorised)
            poly[(size_t) ((n % L) * taps + (taps - 1 - n / L))] = (float) h;
        }
        groupDelay = c / L;                                   // in input samples
        hist.assign ((size_t) (taps + maxIn + 64 + 8), 0.0f);
        reset();
    }

    // extra delay in input samples, realised by the start phase (resolution 1 / L of an input sample); call reset()
    void setExtraDelay (double inSamples) noexcept
    {
        extraUnits = std::max (0LL, (long long) std::llround (inSamples * L));
    }

    void reset() noexcept
    {
        if (identity)
            return;
        std::fill (hist.begin(), hist.end(), 0.0f);
        const int extraZeros = (int) ((extraUnits + L - 1) / L);
        histLen = taps - 1 + extraZeros;
        pos = (long long) (taps - 1 + extraZeros) * L - extraUnits;
    }

    // delay of the filter, in input samples (output n corresponds to input time n M / L - delayIn())
    double delayIn() const noexcept { return identity ? 0.0 : groupDelay + (double) extraUnits / L; }
    bool isIdentity() const noexcept { return identity; }

    // feed n input samples (n <= maxIn), write up to maxOut output samples; returns how many were written
    int process (const float* in, int n, float* out, int maxOut) noexcept
    {
        if (identity)
        {
            const int m = std::min (n, maxOut);
            std::copy (in, in + m, out);
            return m;
        }
        std::copy (in, in + n, hist.begin() + histLen);
        histLen += n;
        int produced = 0;
        while (produced < maxOut)
        {
            const long long i = pos / L;
            if (i >= histLen)
                break;
            const int p = (int) (pos % L);
            out[produced++] = dot (poly.data() + (size_t) (p * taps), hist.data() + i - (taps - 1), taps);
            pos += M;
        }
        const long long drop = pos / L - (taps - 1);
        if (drop > 0)
        {
            std::copy (hist.begin() + drop, hist.begin() + histLen, hist.begin());
            histLen -= (int) drop;
            pos -= drop * L;
        }
        return produced;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    int L = 1, M = 1, taps = 1, histLen = 0;
    long long pos = 0, extraUnits = 0;
    double groupDelay = 0.0;
    bool identity = true;
    std::vector<float> poly, hist;
};
} // namespace axdr
