#pragma once
// Axell core - true-peak measurement: the 8x polyphase interpolator of TruePeakLimiter (Kaiser-windowed sinc, beta 8,
// 8 phases x 128 taps, exact to 0.003 dB up to 0.48 fs) plus parabolic refinement of every local maximum. More accurate
// than the 4x interpolator BS.1770-4 requires as a minimum. JUCE-free; prepare() designs, process() never allocates.
#include "Math.h"

#include <atomic>

namespace axell
{
class TruePeakMeter
{
public:
    static constexpr int kPhases = 8, kTaps = 128;
    static_assert (kTaps % 4 == 0, "interpolate() takes the taps in groups of four");

    using Taps = std::array<std::array<float, kPhases>, kTaps>; // [tap][phase]; phase 0 = the centre sample itself

    // the interpolator: point at centre + p/8 = sum_k w[k] taps[k][p], w[k] = x[now - 127 + k], centre = w[63]
    // the 8 interpolated phases at one position: four independent partial sums (the adds of one long chain would
    // wait for each other; this way they pipeline and vectorise - about 3x faster)
    static inline void interpolate (const float* w, const Taps& taps, float* acc) noexcept
    {
        float s0[kPhases] {}, s1[kPhases] {}, s2[kPhases] {}, s3[kPhases] {};
        for (int k = 0; k < kTaps; k += 4)
        {
            const float* t0 = taps[(size_t) k].data();
            const float* t1 = taps[(size_t) k + 1].data();
            const float* t2 = taps[(size_t) k + 2].data();
            const float* t3 = taps[(size_t) k + 3].data();
            const float w0 = w[k], w1 = w[k + 1], w2 = w[k + 2], w3 = w[k + 3];
            for (int p = 0; p < kPhases; ++p)
            {
                s0[p] += t0[p] * w0;
                s1[p] += t1[p] * w1;
                s2[p] += t2[p] * w2;
                s3[p] += t3[p] * w3;
            }
        }
        for (int p = 0; p < kPhases; ++p)
            acc[p] = (s0[p] + s1[p]) + (s2[p] + s3[p]);
    }

    static void designTaps (Taps& taps)
    {
        constexpr int N = kPhases * kTaps + 1, M = N / 2;
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
        const double beta = 8.0, i0b = bessel0 (beta);
        std::vector<double> h ((size_t) N);
        for (int n = 0; n < N; ++n)
        {
            const double t = (double) (n - M) / kPhases, r = (double) (n - M) / M;
            const double sinc = n == M ? 1.0 : std::sin (kPi * t) / (kPi * t);
            h[(size_t) n] = sinc * bessel0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
        }
        constexpr int centre = kTaps / 2 - 1;
        for (auto& row : taps)
            row.fill (0.0f);
        for (int p = 1; p < kPhases; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const int idx = M + kPhases * (centre - k) + p;
                const double v = (idx >= 0 && idx < N) ? h[(size_t) idx] : 0.0;
                taps[(size_t) k][(size_t) p] = (float) v;
                sum += v;
            }
            for (int k = 0; k < kTaps; ++k)
                taps[(size_t) k][(size_t) p] = (float) (taps[(size_t) k][(size_t) p] / sum);
        }
    }

    void prepare()
    {
        designTaps (taps);
        reset();
    }

    void reset() noexcept
    {
        for (auto& h : hist)
            h.fill (0.0f);
        pos = 0;
        prevU.fill (0.0f);
        prevPrevU.fill (0.0f);
        peak.store (0.0f);
        held = 0.0f;
    }

    // feeds n samples per channel; returns the largest true peak (linear) seen in this call
    float process (const float* const* x, int numChannels, int n) noexcept
    {
        const int nc = std::clamp (numChannels, 1, kMaxCh);
        constexpr int centre = kTaps / 2 - 1;
        float blockMax = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < nc; ++c)
            {
                auto& h = hist[(size_t) c];
                h[(size_t) pos] = h[(size_t) (pos + kTaps)] = x[c][i];
                const float* w = h.data() + pos + 1;
                float acc[kPhases];
                TruePeakMeter::interpolate (w, taps, acc);
                acc[0] = w[centre];
                float a = prevPrevU[(size_t) c], b = prevU[(size_t) c];
                for (int p = 0; p < kPhases; ++p)
                {
                    const float u = std::abs (acc[p]);
                    if (b >= a && b >= u)
                    {
                        const float den = a - 2.0f * b + u;
                        blockMax = std::max (blockMax, den < 0.0f ? b + 0.125f * (a - u) * (a - u) / -den : b);
                    }
                    blockMax = std::max (blockMax, u);
                    a = b;
                    b = u;
                }
                prevPrevU[(size_t) c] = a;
                prevU[(size_t) c] = b;
            }
            pos = (pos + 1) % kTaps;
        }
        held = std::max (held, blockMax);
        peak.store (held, std::memory_order_relaxed);
        return blockMax;
    }

    // clears the held maximum but keeps the filter history (a new measurement of a continuing signal)
    void clearPeak() noexcept
    {
        held = 0.0f;
        peak.store (0.0f, std::memory_order_relaxed);
    }
    float maxTruePeak() const noexcept { return peak.load (std::memory_order_relaxed); } // linear, since reset
    static int latency() noexcept { return kTaps / 2; }

private:
    Taps taps {};
    std::array<std::array<float, 2 * kTaps>, kMaxCh> hist {};
    std::array<float, kMaxCh> prevU {}, prevPrevU {};
    int pos = 0;
    float held = 0.0f;
    std::atomic<float> peak { 0.0f };
};
} // namespace axell
