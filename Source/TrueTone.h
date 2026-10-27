#pragma once
// Axell De-Reverb 4.0 - TRUE TONE.
// De-Reverb's network removes the room, and in small rooms a little more than the room's share of the low end: against
// the voice's direct sound (simulated rooms like the artist's, held-out voices) its output is thinner and brighter, by
// ~3.7 dB at 63 Hz and +1-1.5 dB above 400 Hz on average. How much too much depends on the room - and the plugin can
// see how much it removes in each band. TRUE TONE watches exactly that (its own output vs its input per third octave,
// averaged over ~10 s, relative to the full-band gain) and turns it into a correction curve with a learned linear model
// (TrueToneModel.h), capped at +-3 dB, applied to the processed signal only:
//   Studio: the network's output spectrum is weighted per bin before the inverse FFT (no extra latency);
//   Live:   the per-bin response of the minimum-phase filter is weighted (no latency).
// The curve moves slowly (it is re-evaluated every 8 hops = 85 ms and smoothed over ~0.5 s) and fades in over the first
// ~3 s of signal. Real-time safe: prepare() sizes everything; observe() / update() never allocate.
#include "TrueToneModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace axdr
{
class TrueTone
{
public:
    using cf = std::complex<float>;

    // nb = the number of one-sided bins of an FFT of size nfft at rate fs
    void prepare (int nb, int nfft, double fs)
    {
        using namespace truetone;
        bandOf.assign ((size_t) nb, -1);
        pos.assign ((size_t) nb, 0.0f);
        gain.assign ((size_t) nb, 1.0f);
        const float lo = std::log (kCentres[0]), hi = std::log (kCentres[kBands - 1]);
        for (int k = 0; k < nb; ++k)
        {
            const double f = (double) k * fs / nfft;
            // every bin from 40 Hz to the top band's upper edge counts for its nearest band centre (log frequency): at
            // this FFT size the lowest third octaves would otherwise get no bin at all
            if (f >= 40.0 && f < kCentres[kBands - 1] * std::pow (2.0, 1.0 / 6.0))
            {
                int best = 0;
                for (int b = 1; b < kBands; ++b)
                    if (std::abs (std::log (f / kCentres[b])) < std::abs (std::log (f / kCentres[best])))
                        best = b;
                bandOf[(size_t) k] = best;
            }
            // the bin's place on the band-centre grid (log frequency), for interpolating the curve; held at the ends
            const float lf = std::clamp (std::log ((float) std::max (f, 1.0)), lo, hi);
            float p = 0.0f;
            for (int b = 0; b + 1 < kBands; ++b)
                if (lf >= std::log (kCentres[b]) && lf <= std::log (kCentres[b + 1]))
                {
                    p = (float) b + (lf - std::log (kCentres[b])) / (std::log (kCentres[b + 1]) - std::log (kCentres[b]));
                    break;
                }
            pos[(size_t) k] = lf >= hi ? (float) (kBands - 1) : p;
        }
        alpha = (float) std::exp (-(double) kHopSec / kAvgSec);
        reset();
    }

    void reset() noexcept
    {
        avgX.fill (0.0); avgY.fill (0.0); frameX.fill (0.0); frameY.fill (0.0);
        totX = totY = fTotX = fTotY = 0.0;
        signalHops = 0;
        hopCount = 0;
        std::fill (gain.begin(), gain.end(), 1.0f);
        curve.fill (0.0f);
    }

    // one channel's frame: the input spectrum and the network's (uncorrected) output; channels add up (mono analysis)
    void observe (const cf* X, const cf* Y, int nb) noexcept
    {
        for (int k = 0; k < nb; ++k)
        {
            const double px = (double) std::norm (X[k]), py = (double) std::norm (Y[k]);
            fTotX += px;
            fTotY += py;
            const int b = bandOf[(size_t) k];
            if (b >= 0)
            {
                frameX[(size_t) b] += px;
                frameY[(size_t) b] += py;
            }
        }
    }

    // once per hop, after every channel's observe(); `amount` = the current REDUCTION (0..1): the model for the output
    // that is actually heard (the dry part mixed in) is interpolated between the fitted amounts, and fades to nothing
    // under the lowest one
    void update (int nb, float amount) noexcept
    {
        using namespace truetone;
        const bool signal = fTotX > kSignalPower * nb;
        if (signal)
        {
            // long-term averages over the frames with signal only (a pause does not dilute what the voice taught)
            for (int b = 0; b < kBands; ++b)
            {
                avgX[(size_t) b] = alpha * avgX[(size_t) b] + (1.0 - alpha) * frameX[(size_t) b];
                avgY[(size_t) b] = alpha * avgY[(size_t) b] + (1.0 - alpha) * frameY[(size_t) b];
            }
            totX = alpha * totX + (1.0 - alpha) * fTotX;
            totY = alpha * totY + (1.0 - alpha) * fTotY;
            signalHops = std::min (signalHops + 1, kFadeHops);
        }
        frameX.fill (0.0); frameY.fill (0.0);
        fTotX = fTotY = 0.0;
        if (++hopCount < kEvalHops)
            return;
        hopCount = 0;
        if (signalHops == 0 || totX <= 0.0 || totY <= 0.0)
            return;
        // G: the processing's own gain per band, relative to its full-band gain (dB)
        std::array<float, kBands> g {};
        const double full = 10.0 * std::log10 (totY / totX);
        for (int b = 0; b < kBands; ++b)
            g[(size_t) b] = (avgX[(size_t) b] > 0.0 && avgY[(size_t) b] > 0.0)
                                ? (float) (10.0 * std::log10 (avgY[(size_t) b] / avgX[(size_t) b]) - full) : 0.0f;
        gLast = g;
        const float fadeIn = (float) signalHops / (float) kFadeHops;
        const float a = std::clamp (amount, 0.0f, 1.0f);
        int m0 = 0;
        while (m0 + 2 < kMixCount && a > kMixes[m0 + 1])
            ++m0;
        const float t = std::clamp ((a - kMixes[m0]) / (kMixes[m0 + 1] - kMixes[m0]), 0.0f, 1.0f);
        const float under = a < kMixes[0] ? a / kMixes[0] : 1.0f;     // fades out below the lowest fitted amount
        for (int b = 0; b < kBands; ++b)
        {
            float c0 = kW[m0][0][b], c1 = kW[m0 + 1][0][b];
            for (int k = 0; k < kBands; ++k)
            {
                c0 += kW[m0][1 + k][b] * g[(size_t) k];
                c1 += kW[m0 + 1][1 + k][b] * g[(size_t) k];
            }
            const float c = a < kMixes[0] ? c0 : c0 + t * (c1 - c0);
            curve[(size_t) b] = std::clamp (c, -kCapDb, kCapDb) * fadeIn * under;
        }
        for (int k = 0; k < nb; ++k)
        {
            const float p = pos[(size_t) k];
            const int i0 = std::min ((int) p, kBands - 2);
            const float t = p - (float) i0, db = curve[(size_t) i0] + t * (curve[(size_t) i0 + 1] - curve[(size_t) i0]);
            const float target = std::pow (10.0f, db / 20.0f);
            gain[(size_t) k] += kSmooth * (target - gain[(size_t) k]);
        }
    }

    float binGain (int k) const noexcept { return gain[(size_t) k]; }
    float curveDb (int b) const noexcept { return curve[(size_t) b]; }
    float measuredDb (int b) const noexcept { return gLast[(size_t) b]; }   // G, the last evaluation (diagnostics)

    static constexpr float kCapDb = 3.0f;

private:
    static constexpr double kHopSec = 512.0 / 48000.0, kAvgSec = 10.0, kSignalPower = 1.0e-9;
    static constexpr int kEvalHops = 8, kFadeHops = 280;           // ~3 s of signal to full strength
    static constexpr float kSmooth = 0.15f;                       // per evaluation (85 ms): ~0.5 s
    std::vector<int> bandOf;
    std::vector<float> pos, gain;
    std::array<double, truetone::kBands> avgX {}, avgY {}, frameX {}, frameY {};
    std::array<float, truetone::kBands> curve {}, gLast {};
    double totX = 0, totY = 0, fTotX = 0, fTotY = 0, alpha = 0.999;
    int signalHops = 0, hopCount = 0;
};
} // namespace axdr
