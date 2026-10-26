#pragma once
// Axell core (from Axell Gate 2.0) - room model for voice detection (framework independent, no JUCE). 2.0.
//
// In an untreated room every word rings on: a tail that is quiet next to the voice but loud next to the room tone,
// periodic (it is the voice) and low in frequency (bare rooms ring longest in the bass). A detector that only sees
// the frame cannot tell it from a soft voiced sound. The room can: after the excitation stops, the energy in every
// band can only fall at the room's own decay rate. So:
//  1. the decay rate of each band is learned from the late phase of the free decays the room produces (after a
//     word stops, past the direct sound's own fast drop, the band falls at the room's rate) - blind, continuously,
//     no setup;
//  2. every frame, the late reverberation is predicted from the band energy D = 40 ms earlier, decayed at the
//     room's rate (the late-reverberant spectral variance estimator of Lebart et al. 2001 / Habets 2009, used here
//     as detection evidence instead of for suppression);
//  3. what the room cannot explain - the excess over the prediction, above the band's floor - is new excitation:
//     a word start, a held vowel, a consonant. A tail has (almost) none.
// Outputs per frame: the excess as a fraction of the energy above the floor, its level above the floor, and the
// room's mean RT60. Real-time safe: prepare() allocates; push() never allocates.
#include <axell/dsp/Filters.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace axell::voice
{
class RoomModel
{
public:
    static constexpr int kBands = 4;              // 60-300 Hz, 300 Hz-1 kHz, 1-4 kHz, 4-12 kHz
    static constexpr int kRing = 64;              // hops of band power kept (128 ms at 2 ms)
    static constexpr int kBins = 64;              // decay-rate histogram: 20 .. 1300 dB/s, log spaced
    static constexpr double kDelayS = 0.04;       // the "late" boundary of the prediction
    static constexpr double kFitS = 0.04;         // a free decay is judged over this span

    struct Out
    {
        float excess = 0.0f;      // 0..1: share of the energy above the floor the room cannot explain
        float excessDb = -20.0f;  // the unexplained energy over the floor (dB)
        float rt60 = 0.3f;        // the room's mean RT60 (s), as learned
        float decay = 0.0f;       // how fast the frame is falling, re the room's own rate over the last 30 ms
                                  // (1: exactly as the room rings out, 0: steady, < 0: rising), energy-weighted
    };

    void prepare (double sampleRate, int hopSamples)
    {
        fs = sampleRate;
        hop = std::max (1, hopSamples);
        static constexpr double edges[kBands + 1] = { 60.0, 300.0, 1000.0, 4000.0, 12000.0 };
        for (int b = 0; b < kBands; ++b)
        {
            hp[(size_t) b].setHighPass (fs, edges[b], 0.7071);
            lp[(size_t) b].setLowPass (fs, std::min (edges[b + 1], 0.45 * fs), 0.7071);
        }
        delayHops = std::max (1, (int) std::lround (kDelayS * fs / hop));
        fitHops = std::max (2, (int) std::lround (kFitS * fs / hop));
        decHops = std::max (2, (int) std::lround (0.03 * fs / hop));
        for (int i = 0; i < kBins; ++i)
            binRate[(size_t) i] = 20.0 * std::pow (1300.0 / 20.0, (i + 0.5) / kBins);
        reset();
    }

    void reset() noexcept
    {
        for (int b = 0; b < kBands; ++b)
        {
            hp[(size_t) b].resetState();
            lp[(size_t) b].resetState();
            acc[(size_t) b] = 0.0;
            smooth[(size_t) b] = 0.0;
            floorDb[(size_t) b] = -140.0;
            floorSet[(size_t) b] = false;
            for (auto& v : ring[(size_t) b])
                v = 0.0;
            // prior: a typical small room (RT60 0.3 s = 200 dB/s), worth ~20 observed decays
            hist[(size_t) b].fill (0.0);
            hist[(size_t) b][(size_t) binOf (200.0)] = 20.0;
            rate[(size_t) b] = 200.0;
        }
        count = 0;
        pos = 0;
        hops = 0;
    }

    // one (high-passed) input sample
    void push (float x) noexcept
    {
        for (int b = 0; b < kBands; ++b)
        {
            const double y = lp[(size_t) b].process (hp[(size_t) b].process (x, 0), 0);
            acc[(size_t) b] += y * y;
        }
        if (++count >= hop)
            endHop();
    }

    // the room's verdict on the hop `back` hops ago (aligned with the voice detector's frame centre)
    Out at (int back) const noexcept
    {
        Out o;
        back = std::clamp (back, 0, kRing - std::max (delayHops, decHops) - 1);
        const int i = (pos - 1 - back + 2 * kRing) % kRing, j = (i - delayHops + kRing) % kRing;
        double ex = 0.0, above = 0.0, fl = 0.0;
        for (int b = 0; b < kBands; ++b)
        {
            const double f = std::pow (10.0, floorDb[(size_t) b] / 10.0);
            const double p = std::max (0.0, ring[(size_t) b][(size_t) i] - f);
            const double late = std::max (0.0, ring[(size_t) b][(size_t) j] - f) * std::pow (10.0, -rate[(size_t) b] * kDelayS / 10.0);
            ex += std::max (0.0, p - 2.0 * late); // 3 dB of margin: what the room surely cannot explain
            above += p;
            fl += f;
        }
        o.excess = above > 1.0e-30 ? (float) std::clamp (ex / above, 0.0, 1.0) : 0.0f;
        const int k30 = (i - decHops + kRing) % kRing;
        double dw = 0.0, dsum = 0.0;
        for (int b = 0; b < kBands; ++b)
        {
            const double f = std::pow (10.0, floorDb[(size_t) b] / 10.0);
            const double now = ring[(size_t) b][(size_t) i], then = ring[(size_t) b][(size_t) k30];
            const double w = std::max (0.0, then - f);
            if (w <= 0.0)
                continue;
            const double fall = 10.0 * std::log10 ((then + 1.0e-20) / (now + 1.0e-20)) / (rate[(size_t) b] * decHops * hop / fs);
            dsum += w * std::clamp (fall, -1.0, 2.0);
            dw += w;
        }
        o.decay = dw > 0.0 ? (float) (dsum / dw) : 0.0f;
        o.excessDb = (float) std::clamp (10.0 * std::log10 ((ex + 1.0e-30) / (fl + 1.0e-30)), -20.0, 80.0);
        double r = 0.0;
        for (int b = 0; b < kBands; ++b)
            r += rate[(size_t) b];
        o.rt60 = (float) (60.0 / (r / kBands));
        return o;
    }

    float bandRt60 (int b) const noexcept { return (float) (60.0 / rate[(size_t) std::clamp (b, 0, kBands - 1)]); }

private:
    int binOf (double r) const noexcept
    {
        return std::clamp ((int) std::floor (std::log (r / 20.0) / std::log (1300.0 / 20.0) * kBins), 0, kBins - 1);
    }

    void endHop() noexcept
    {
        count = 0;
        ++hops;
        for (int b = 0; b < kBands; ++b)
        {
            const double p = acc[(size_t) b] / hop;
            acc[(size_t) b] = 0.0;
            smooth[(size_t) b] += (p - smooth[(size_t) b]) * 0.5; // ~4 ms
            ring[(size_t) b][(size_t) pos] = smooth[(size_t) b];
            const double d = 10.0 * std::log10 (smooth[(size_t) b] + 1.0e-20);
            // band floor: a slowly rising minimum (3 dB/s). Digital silence (before a take, between items) is not a floor:
            // it would leave the floor at -200 dB for a minute and make every frame look far above the room
            if (d < -140.0)
                continue;
            if (! floorSet[(size_t) b])
                floorDb[(size_t) b] = d;
            else
                floorDb[(size_t) b] = d < floorDb[(size_t) b] ? d : floorDb[(size_t) b] + 3.0 * hop / fs;
            floorSet[(size_t) b] = true;
        }
        // free decays, late phase: the band has been falling steadily for 80 ms; over the last 40 ms (after the
        // direct sound's own drop, which is far steeper than the room) it fell 6 dB or more, from 14 dB or more above
        // its floor to 6 dB above it
        if (hops > 2 * fitHops + 1)
            for (int b = 0; b < kBands; ++b)
            {
                const auto& rg = ring[(size_t) b];
                auto lev = [&] (int k) { return 10.0 * std::log10 (rg[(size_t) ((pos - k + 2 * kRing) % kRing)] + 1.0e-20); };
                const double fl = floorDb[(size_t) b];
                // the decay was already under way 40 ms earlier (past the direct sound's drop) ...
                if (lev (2 * fitHops) - lev (fitHops) < 3.0 || lev (fitHops) - fl < 14.0 || lev (0) - fl < 6.0)
                    continue;
                // ... and the last 40 ms are a straight line in dB (least squares; residual under 1.5 dB RMS)
                double sx = 0, sy = 0, sxx = 0, sxy = 0;
                const int m = fitHops + 1;
                for (int k = 0; k < m; ++k)
                {
                    const double xk = -k * hop / fs, yk = lev (k);
                    sx += xk; sy += yk; sxx += xk * xk; sxy += xk * yk;
                }
                const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx), icpt = (sy - slope * sx) / m;
                double res = 0;
                for (int k = 0; k < m; ++k)
                {
                    const double e = lev (k) - (icpt + slope * (-k * hop / fs));
                    res += e * e;
                }
                if (std::sqrt (res / m) > 1.5 || -slope < 30.0)
                    continue;
                const double r = -slope; // dB/s
                auto& h = hist[(size_t) b];
                for (auto& v : h)
                    v *= 0.995; // the room is re-learned over the last ~200 decays
                h[(size_t) binOf (r)] += 1.0;
                // the room's rate: the 60th percentile of the late decays (a word can fade slower than its room, never
                // faster; measured: within ~15 % of the true RT60 on synthetic rooms and a real one)
                double tot = 0.0;
                for (double v : h)
                    tot += v;
                double c = 0.0;
                for (int k = 0; k < kBins; ++k)
                {
                    c += h[(size_t) k];
                    if (c >= 0.6 * tot)
                    {
                        rate[(size_t) b] = binRate[(size_t) k];
                        break;
                    }
                }
            }
        pos = (pos + 1) % kRing;
    }

    double fs = 48000.0;
    int hop = 96, delayHops = 20, fitHops = 20, decHops = 15, count = 0, pos = 0;
    long long hops = 0;
    std::array<bool, kBands> floorSet {};
    std::array<axell::Biquad, kBands> hp, lp;
    std::array<double, kBands> acc {}, smooth {}, floorDb {}, rate {};
    std::array<std::array<double, kRing>, kBands> ring {};
    std::array<std::array<double, kBins>, kBands> hist {};
    std::array<double, kBins> binRate {};
};
} // namespace axell::voice
