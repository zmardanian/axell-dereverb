#pragma once
// Axell core - true-peak brickwall limiter (from Axell Duo Compressor 2.0, its DECISIONS D21)
// JUCE-free; tested with plain g++ (tests/). Shared by the Axell plugins.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Math.h"
#include "SlidingMax.h"

namespace axell
{
//==============================================================================
// True-peak brickwall limiter (2.0), base rate, stereo-linked. DECISIONS D21.
//
// 1. True-peak detector: 8x polyphase interpolation (Kaiser-windowed sinc, beta 8, 8 phases x 128 taps)
//    plus parabolic refinement of every local maximum. Interpolation error <= 0.003 dB for content up to
//    0.48 fs (21.2 kHz at 44.1 kHz, 23.1 kHz at 48 kHz) against an exact band-limited reference; closer
//    to Nyquist no low-latency interpolator is exact (DECISIONS D21). peak[b] = the largest true peak in
//    (b - 1, b + 1); the ceiling carries a fixed design margin (kMarginDb).
// 2. Required gain r[b] = min(1, ceiling / peak[b]) -> running minimum over the lookahead L -> release
//    (rises with the release time constant, falls instantly) -> kSmooth cascaded moving averages whose
//    combined support is L. Every value that enters the smoothing for sample b is <= r[b]: the gain never
//    exceeds what sample b and its neighbourhood need. One average would ramp linearly, and the corners of
//    that ramp modulate high frequencies into new inter-sample peaks (measured up to +0.02 dB); the
//    cascade's smooth S-curve does not (DECISIONS D21).
// Latency: kTaps / 2 + L samples. prepare() allocates; everything else is RT-safe.
class TruePeakLimiter
{
public:
    static constexpr int kPhases = 8, kTaps = 128;
    static constexpr int kSmooth = 3;
    static constexpr int latencyFor (int lookaheadSamples) noexcept { return kTaps / 2 + std::max (kSmooth, lookaheadSamples); }

    void prepare (double sampleRate, int maxLookahead)
    {
        fs = sampleRate;
        maxLook = std::max (1, maxLookahead);
        designInterpolator();
        int cap = 1;
        while (cap < maxLook + kTaps + 4)
            cap <<= 1;
        ringMask = cap - 1;
        for (auto& d : delay)
            d.assign ((size_t) cap, 0.0f);
        for (auto& r : avgRing)
            r.assign ((size_t) cap, 1.0);
        attMax.prepare (maxLook);
        setLookahead (std::min (maxLook, (int) std::lround (0.0015 * fs)));
        setParams (-1.0f, 50.0f);
        resetState();
    }

    // structural (changes the latency): call while the output is muted, then resetState()
    void setLookahead (int samples) noexcept
    {
        look = std::clamp (samples, kSmooth, maxLook);
        attMax.setWindow (look);
        // lengths with sum(len) - (kSmooth - 1) == look
        int rest = look + kSmooth - 1;
        for (int k = 0; k < kSmooth; ++k)
        {
            avgLen[(size_t) k] = rest / (kSmooth - k);
            rest -= avgLen[(size_t) k];
        }
    }
    int lookahead() const noexcept { return look; }
    int latency() const noexcept { return latencyFor (look); }

    void setParams (float ceilingDb, float releaseMs) noexcept
    {
        ceiling = std::pow (10.0f, (std::clamp (ceilingDb, -24.0f, 0.0f) - kMarginDb) / 20.0f);
        relK = 1.0f - onePole (std::clamp (releaseMs, 1.0f, 1000.0f) * 0.001f, fs);
    }

    void resetState() noexcept
    {
        for (auto& h : hist)
            h.fill (0.0f);
        histPos = 0;
        for (auto& d : delay)
            std::fill (d.begin(), d.end(), 0.0f);
        for (int k = 0; k < kSmooth; ++k)
        {
            std::fill (avgRing[(size_t) k].begin(), avgRing[(size_t) k].end(), 1.0);
            avgSum[(size_t) k] = (double) avgLen[(size_t) k];
        }
        pos = 0;
        prevU.fill (0.0f);
        prevPrevU.fill (0.0f);
        openPeak = completedPrev = 0.0f;
        attMax.resetState();
        env = 1.0f;
        gainNow = 1.0f;
    }

    // in place, n base-rate frames; returns the deepest gain reduction in dB in this call
    float process (float* const* io, int numCh, int n) noexcept
    {
        float minGain = 1.0f;
        const int nc = std::clamp (numCh, 1, kMaxCh);
        constexpr int centre = kTaps / 2 - 1;
        for (int i = 0; i < n; ++i)
        {
            // --- detector. w[k] = x[now - 63 + k]; this step yields the points c + p/8 (p = 0..7) of the
            //     interval [c, c + 1), c = now - 32. A local maximum is refined when its successor arrives;
            //     the successor of the p = 7 point is this step's p = 0 point, so interval c - 1 completes now.
            float thisPeak = 0.0f, lateForPrev = 0.0f;
            for (int c = 0; c < nc; ++c)
            {
                auto& h = hist[(size_t) c];
                const float v = io[c][i];
                h[(size_t) histPos] = v;
                h[(size_t) (histPos + kTaps)] = v;
                const float* w = h.data() + histPos + 1;
                float a = prevPrevU[(size_t) c], b = prevU[(size_t) c];
                // all 8 phases at once (vectorises); phase 0 is a delta: the centre sample itself
                float acc[kPhases] {};
                for (int k = 0; k < kTaps; ++k)
                {
                    const float wk = w[k];
                    const float* t = tapsT[(size_t) k].data();
                    for (int p = 0; p < kPhases; ++p)
                        acc[p] += t[p] * wk;
                }
                acc[0] = w[centre];
                for (int p = 0; p < kPhases; ++p)
                {
                    const float u = std::abs (acc[p]);
                    if (b >= a && b >= u)
                    {
                        const float den = a - 2.0f * b + u;
                        const float pk = den < 0.0f ? b + 0.125f * (a - u) * (a - u) / -den : b;
                        (p == 0 ? lateForPrev : thisPeak) = std::max (p == 0 ? lateForPrev : thisPeak, pk);
                    }
                    thisPeak = std::max (thisPeak, u);
                    a = b;
                    b = u;
                }
                prevPrevU[(size_t) c] = a;
                prevU[(size_t) c] = b;
            }
            histPos = (histPos + 1) % kTaps;
            const float completed = std::max (openPeak, lateForPrev);   // interval [now - 33, now - 32)
            const float peak = std::max (completedPrev, completed);     // (b - 1, b + 1), b = now - 33
            completedPrev = completed;
            openPeak = thisPeak;

            // --- gain computer (for sample b)
            const float att = peak > ceiling ? peak / ceiling : 1.0f;
            const float need = 1.0f / attMax.process (att, 0);         // running min of the gain over L
            env = need < env ? need : std::min (need, env + (need - env) * relK);
            const int ap = pos & ringMask;
            double g = env;
            for (int k = 0; k < kSmooth; ++k)
            {
                auto& r = avgRing[(size_t) k];
                const int len = avgLen[(size_t) k];
                avgSum[(size_t) k] += g - r[(size_t) ((pos - len) & ringMask)];
                r[(size_t) ap] = g;
                g = avgSum[(size_t) k] / (double) len;
            }
            gainNow = std::min (1.0f, (float) g);
            minGain = std::min (minGain, gainNow);

            // --- audio: the average just computed belongs to sample b - (L - 1) = now - (32 + L)
            for (int c = 0; c < nc; ++c)
            {
                auto& d = delay[(size_t) c];
                d[(size_t) ap] = io[c][i];
                io[c][i] = d[(size_t) ((pos - latency()) & ringMask)] * gainNow;
            }
            ++pos;
        }
        return minGain < 1.0f ? -20.0f * std::log10 (minGain) : 0.0f;
    }

    float currentGain() const noexcept { return gainNow; }

private:
    void designInterpolator()
    {
        constexpr int N = kPhases * kTaps + 1, M = N / 2; // h[n] = sinc((n - M) / 8) * kaiser
        auto bessel0 = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 60; ++k)
            {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        constexpr double beta = 8.0;
        const double i0b = bessel0 (beta);
        std::vector<double> h ((size_t) N, 0.0);
        for (int n = 0; n < N; ++n)
        {
            const double t = (double) (n - M) / kPhases;
            const double sinc = n == M ? 1.0 : std::sin (kPi * t) / (kPi * t);
            const double r = (double) (n - M) / M;
            h[(size_t) n] = sinc * bessel0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
        }
        // point at centre + p/8 = sum_k w[k] h[M + 8 (centre - k) + p]
        constexpr int centre = kTaps / 2 - 1;
        for (int p = 1; p < kPhases; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const int idx = M + kPhases * (centre - k) + p;
                const double v = (idx >= 0 && idx < N) ? h[(size_t) idx] : 0.0;
                tapsT[(size_t) k][(size_t) p] = (float) v;
                sum += v;
            }
            for (int k = 0; k < kTaps; ++k) // unity DC gain per phase
                tapsT[(size_t) k][(size_t) p] = (float) (tapsT[(size_t) k][(size_t) p] / sum);
        }
    }

    static constexpr float kMarginDb = 0.05f;
    double fs = 44100.0;
    int maxLook = 66, look = 66, ringMask = 0, pos = 0, histPos = 0;
    std::array<std::array<float, kPhases>, kTaps> tapsT {}; // [tap][phase]
    std::array<std::array<float, 2 * kTaps>, kMaxCh> hist {};
    std::array<std::vector<float>, kMaxCh> delay;
    std::array<std::vector<double>, kSmooth> avgRing;
    std::array<double, kSmooth> avgSum {};
    std::array<int, kSmooth> avgLen {};
    std::array<float, kMaxCh> prevU {}, prevPrevU {};
    float openPeak = 0.0f, completedPrev = 0.0f;
    SlidingMax attMax;
    float ceiling = 0.891f, relK = 0.001f, env = 1.0f, gainNow = 1.0f;
};
} // namespace axell
