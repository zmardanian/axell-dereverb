#pragma once
// Axell core - numeric helpers: fast dB conversion, smoothing coefficients, static compressor curve
// JUCE-free; tested with plain g++ (tests/). Shared by the Axell plugins.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace axell
{
constexpr int kMaxCh = 2;
constexpr double kPi = 3.141592653589793;
constexpr int kNumOsFactors = 3; // 1x, 2x, 4x

//==============================================================================
// Fast, deterministic dB conversions (called per oversampled sample).
// fastLog2: |error| < 1e-6 (=> < 1e-5 dB). fastExp2: relative error < 2e-7, exp2(0) == 1 exactly.
inline float fastLog2 (float x) noexcept
{
    uint32_t bits;
    std::memcpy (&bits, &x, sizeof bits);
    int e = (int) ((bits >> 23) & 0xffu) - 127;
    bits = (bits & 0x007fffffu) | 0x3f800000u;
    float m;
    std::memcpy (&m, &bits, sizeof m);
    if (m > 1.41421356f)
    {
        m *= 0.5f;
        ++e;
    }
    const float t = (m - 1.0f) / (m + 1.0f);
    const float t2 = t * t;
    const float s = t * (1.0f + t2 * (0.333333333f + t2 * (0.2f + t2 * 0.142857143f)));
    return (float) e + 2.88539008f * s;
}

inline float fastExp2 (float x) noexcept
{
    x = std::clamp (x, -126.0f, 126.0f);
    const float r = std::floor (x + 0.5f);
    const float f = (x - r) * 0.693147181f; // |f| <= 0.3466
    const float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.166666667f + f * (0.0416666667f + f * (0.00833333333f + f * 0.00138888889f)))));
    const uint32_t bits = (uint32_t) ((int) r + 127) << 23;
    float scale;
    std::memcpy (&scale, &bits, sizeof scale);
    return p * scale;
}

inline float dbToGain (float db) noexcept { return fastExp2 (db * 0.166096405f); }
inline float gainToDb (float g) noexcept { return 6.02059991f * fastLog2 (std::max (g, 1.0e-9f)); }
inline float powerToDb (float p) noexcept { return 3.01029996f * fastLog2 (std::max (p, 1.0e-18f)); }

// one-pole smoothing coefficient for a time constant in seconds (computed at block rate)
inline float onePole (float seconds, double fs) noexcept
{
    return seconds <= 0.0f ? 0.0f : (float) std::exp (-1.0 / ((double) seconds * fs));
}

// 1 - exp(-x) for the small per-sample rates of slow envelopes (x < 0.01): avoids a
// transcendental per sample and lets release times change smoothly every sample.
inline float oneMinusExpSmall (float x) noexcept { return x * (1.0f - x * (0.5f - x * 0.166666667f)); }

// static soft-knee curve, returns gain reduction in dB (positive number)
inline float gainReductionDb (float levelDb, float thresholdDb, float ratio, float kneeDb) noexcept
{
    const float over  = levelDb - thresholdDb;
    const float slope = 1.0f - 1.0f / ratio;

    if (kneeDb > 0.0f)
    {
        if (2.0f * over < -kneeDb)
            return 0.0f;
        if (2.0f * std::abs (over) <= kneeDb)
        {
            const float t = over + kneeDb * 0.5f;
            return slope * t * t / (2.0f * kneeDb);
        }
    }
    else if (over <= 0.0f)
    {
        return 0.0f;
    }
    return slope * over;
}

} // namespace axell
