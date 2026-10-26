#pragma once
// Axell core - a magnitude response made into a causal, zero-latency filter (from Axell Velvet 3.0's LIVE de-esser).
// Real cepstrum (Oppenheim & Schafer): log|H| -> cepstrum -> fold onto n >= 0 -> exp -> the minimum-phase filter, its
// energy at the front, no delay. MinPhaseFir applies a new filter every hop, crossfading from the old one over the hop
// so a gain change never clicks. Used by Velvet 3.0 LIVE (its own copy) and Axell De-Reverb 3.0 LIVE.
// Real-time safe: prepare() allocates; design() and process() never allocate, lock or throw.
#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace axell
{
class MinPhaseDesigner
{
public:
    // nfft: the design grid (the magnitude has nfft / 2 + 1 bins); taps: the filter length kept (<= nfft / 2)
    void prepare (int nfft, int numTaps)
    {
        n = nfft;
        taps = std::clamp (numTaps, 1, n / 2);
        fft.prepare (n);
        spec.assign ((size_t) n + 2, 0.0f);
        cep.assign ((size_t) n, 0.0f);
        h.assign ((size_t) n, 0.0f);
        taper.assign ((size_t) taps, 1.0f);
        const int nt = std::max (1, taps / 6);                 // raised-cosine fade of the last sixth: no truncation step
        for (int i = 0; i < nt; ++i)
            taper[(size_t) (taps - nt + i)] = (float) (0.5 + 0.5 * std::cos (3.14159265358979323846 * (i + 0.5) / nt));
    }
    int bins() const noexcept { return n / 2 + 1; }
    int numTaps() const noexcept { return taps; }

    // mag: bins() linear magnitudes (>= 0); out: numTaps() samples of the minimum-phase impulse response
    void design (const float* mag, float* out) noexcept
    {
        const int H = n / 2;
        for (int k = 0; k <= H; ++k)
        {
            spec[(size_t) (2 * k)] = std::log (std::max (mag[k], 1.0e-5f));
            spec[(size_t) (2 * k + 1)] = 0.0f;
        }
        fft.inverse (spec.data(), cep.data());               // real cepstrum (even)
        for (int i = 1; i < H; ++i)
            cep[(size_t) i] *= 2.0f;
        std::fill (cep.begin() + H + 1, cep.end(), 0.0f);
        fft.forward (cep.data(), spec.data());               // log H_min
        for (int k = 0; k <= H; ++k)
        {
            const float m = std::exp (spec[(size_t) (2 * k)]), ph = spec[(size_t) (2 * k + 1)];
            spec[(size_t) (2 * k)] = m * std::cos (ph);
            spec[(size_t) (2 * k + 1)] = m * std::sin (ph);
        }
        fft.inverse (spec.data(), h.data());
        for (int i = 0; i < taps; ++i)
            out[i] = h[(size_t) i] * taper[(size_t) i];
    }

private:
    int n = 1024, taps = 256;
    RealFft fft;
    std::vector<float> spec, cep, h, taper;
};

// a time-varying FIR: setFilter() takes the next filter, which fades in linearly over `fadeLen` samples
class MinPhaseFir
{
public:
    void prepare (int numTaps, int fadeLen)
    {
        taps = numTaps;
        fade = std::max (1, fadeLen);
        hist.assign ((size_t) (2 * taps), 0.0f);
        rOld.assign ((size_t) taps, 0.0f);
        rNew.assign ((size_t) taps, 0.0f);
        reset();
    }
    void reset() noexcept
    {
        if (rNew.empty())                                      // never prepared: nothing to reset
            return;
        std::fill (hist.begin(), hist.end(), 0.0f);
        std::fill (rOld.begin(), rOld.end(), 0.0f);
        std::fill (rNew.begin(), rNew.end(), 0.0f);
        rOld[(size_t) taps - 1] = rNew[(size_t) taps - 1] = 1.0f;   // identity until the first filter
        pos = 0;
        ramp = fade;
    }
    // h: numTaps() samples of the impulse response (h[0] applies to the current sample)
    void setFilter (const float* h) noexcept
    {
        std::swap (rOld, rNew);
        if (ramp < fade)                                       // a fade still running: start from where it is
        {
            const float w = (float) ramp / (float) fade;
            for (int i = 0; i < taps; ++i)
                rOld[(size_t) i] = rNew[(size_t) i] + w * (rOld[(size_t) i] - rNew[(size_t) i]);
        }
        for (int i = 0; i < taps; ++i)                         // stored reversed: oldest input first
            rNew[(size_t) (taps - 1 - i)] = h[i];
        ramp = 0;
    }
    float process (float x) noexcept
    {
        // the history is kept twice: the last `taps` inputs, oldest first, are hist[pos + 1 .. pos + taps]
        hist[(size_t) pos] = hist[(size_t) (pos + taps)] = x;
        const float* xs = hist.data() + pos + 1;
        const float a = dot (rNew.data(), xs, taps);
        pos = (pos + 1) % taps;
        if (ramp >= fade)
            return a;
        const float b = dot (rOld.data(), xs, taps);
        const float w = (float) ramp++ / (float) fade;
        return b + w * (a - b);
    }
    int numTaps() const noexcept { return taps; }

private:
    // 8 independent accumulators: vectorises without fast-math, deterministic
    static float dot (const float* a, const float* b, int n) noexcept
    {
        float s[8] {};
        int i = 0;
        for (; i + 8 <= n; i += 8)
            for (int k = 0; k < 8; ++k)
                s[k] += a[i + k] * b[i + k];
        float t = ((s[0] + s[4]) + (s[1] + s[5])) + ((s[2] + s[6]) + (s[3] + s[7]));
        for (; i < n; ++i)
            t += a[i] * b[i];
        return t;
    }
    int taps = 1, fade = 1, pos = 0, ramp = 1;
    std::vector<float> hist, rOld, rNew;
};
} // namespace axell
