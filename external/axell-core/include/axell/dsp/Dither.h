#pragma once
// Axell core - dither and quantisation to 16 / 24-bit for the final export: TPDF (+-1 LSB triangular) and an optional
// psychoacoustic error-feedback noise shaper. The output is quantised to the target grid, so a host exporting at that bit
// depth stores it exactly.
// Shaping filter (44.1 kHz and up): the 9-coefficient F-weighted minimally audible filter of R. A. Wannamaker,
// "Psychoacoustically Optimal Noise Shaping", J. Audio Eng. Soc. 40(7/8), 1992. Noise transfer 1 - sum h_k z^-k:
// -25 dB at 3 kHz (where hearing is most sensitive), the noise moved above 15 kHz; minimum phase (mean log gain 0 dB).
// Below 44.1 kHz a first-order shaper (1 - z^-1).
#include "Math.h"

namespace axell
{
class Dither
{
public:
    enum class Mode { off, tpdf, shaped };
    static constexpr int kOrder = 9;
    static constexpr double kFWeighted[kOrder] { 2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847 };

    void prepare (double sampleRate) noexcept
    {
        std::fill (std::begin (h), std::end (h), 0.0);
        if (sampleRate >= 44000.0)
            std::copy (std::begin (kFWeighted), std::end (kFWeighted), std::begin (h));
        else
            h[0] = 1.0;
        reset();
    }
    void set (int bits, Mode m) noexcept
    {
        mode = m;
        bitsN = std::clamp (bits, 8, 24);
        lsb = 1.0 / (double) (1 << (bitsN - 1));
    }
    void reset() noexcept
    {
        for (auto& e : err)
            e.fill (0.0);
        state = { 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL };
    }

    void process (float* const* io, int numChannels, int n) noexcept
    {
        if (mode == Mode::off)
            return;
        for (int c = 0; c < std::clamp (numChannels, 1, kMaxCh); ++c)
            for (int i = 0; i < n; ++i)
            {
                double x = io[c][i];
                auto& e = err[(size_t) c];
                if (mode == Mode::shaped)
                    for (int k = 0; k < kOrder; ++k)
                        x -= h[k] * e[(size_t) k];                           // e[0] = previous sample's error
                const double d = (uniform (c) - uniform (c)) * lsb;       // TPDF, +-1 LSB (own generator per channel)
                double q = std::floor ((x + d) / lsb + 0.5) * lsb;          // round to the grid
                q = std::clamp (q, -1.0, 1.0 - lsb);
                if (mode == Mode::shaped)
                {
                    for (int k = kOrder - 1; k > 0; --k)
                        e[(size_t) k] = e[(size_t) k - 1];
                    // total error (quantiser + dither); bounded so a full-scale clamp can never destabilise the loop
                    e[0] = std::clamp (q - x, -4.0 * lsb, 4.0 * lsb);
                }
                io[c][i] = (float) q;
            }
    }

private:
    double uniform (int c) noexcept
    {
        auto& st = state[(size_t) c];
        st = st * 6364136223846793005ULL + 1442695040888963407ULL;           // LCG, deterministic per channel
        return (double) (st >> 11) * (1.0 / 9007199254740992.0);
    }
    Mode mode = Mode::off;
    int bitsN = 24;
    double lsb = 1.0 / 8388608.0;
    std::array<std::array<double, kOrder>, kMaxCh> err {};
    double h[kOrder] { 1.0 };
    std::array<uint64_t, kMaxCh> state { 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL };
};
} // namespace axell
