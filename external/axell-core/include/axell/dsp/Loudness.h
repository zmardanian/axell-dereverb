#pragma once
// Axell core - loudness metering per ITU-R BS.1770-4 / EBU R128 (EBU Tech 3341, 3342).
// JUCE-free. prepare() allocates; process() and the block updates never do.
//
//  - K-weighting per channel (BS.1770 pre-filter + RLB), channel weights 1.0 (L, R)
//  - mean square per 100 ms sub-block; momentary = last 4 sub-blocks (400 ms), short-term = last 30 (3 s)
//  - integrated: 400 ms gating blocks with 75 % overlap (every 100 ms), absolute gate -70 LUFS, relative gate -10 LU
//  - loudness range (EBU 3342): short-term values every 100 ms, absolute gate -70 LUFS, relative gate -20 LU,
//    LRA = 95th - 10th percentile
// Integrated loudness and LRA use histograms with 0.01 LU bins (-70 .. +10 LUFS): any program length, fixed memory;
// the quantisation error is below 0.005 LU.
#include "Filters.h"

#include <atomic>
#include <limits>

namespace axell
{
class LoudnessMeter
{
public:
    static constexpr double kFloor = -70.0, kTop = 10.0, kBin = 0.01;
    static constexpr int kBins = (int) ((kTop - kFloor) / kBin) + 1;

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        nCh = std::clamp (numChannels, 1, kMaxCh);
        subLen = std::max (1, (int) std::lround (0.1 * fs));
        kw.prepare (fs);
        intCount.assign ((size_t) kBins, 0);
        intEnergy.assign ((size_t) kBins, 0.0);
        lraCount.assign ((size_t) kBins, 0);
        lraEnergy.assign ((size_t) kBins, 0.0);
        reset();
    }

    void reset() noexcept
    {
        kw.resetState();
        acc = 0.0;
        accN = 0;
        sub.fill (0.0);
        subPos = 0;
        subFilled = 0;
        std::fill (intCount.begin(), intCount.end(), 0);
        std::fill (intEnergy.begin(), intEnergy.end(), 0.0);
        std::fill (lraCount.begin(), lraCount.end(), 0);
        std::fill (lraEnergy.begin(), lraEnergy.end(), 0.0);
        intTotal = lraTotal = 0;
        momentary.store (kNone);
        shortTerm.store (kNone);
        integrated.store (kNone);
        range.store (0.0f);
        maxMomentary.store (kNone);
        maxShortTerm.store (kNone);
    }

    // interleaving-free: one pointer per channel
    void process (const float* const* x, int numChannels, int n) noexcept
    {
        const int nc = std::clamp (numChannels, 1, nCh);
        for (int i = 0; i < n; ++i)
        {
            double s = 0.0;
            for (int c = 0; c < nc; ++c)
            {
                const double v = kw.process (x[c][i], c);
                s += v * v;
            }
            acc += s;
            if (++accN >= subLen)
                endSubBlock();
        }
    }

    // thread-safe reads (LUFS / LU); kNone = not enough audio yet
    static constexpr float kNone = -std::numeric_limits<float>::infinity();
    float momentaryLufs() const noexcept { return momentary.load (std::memory_order_relaxed); }
    float shortTermLufs() const noexcept { return shortTerm.load (std::memory_order_relaxed); }
    float integratedLufs() const noexcept { return integrated.load (std::memory_order_relaxed); }
    float loudnessRangeLu() const noexcept { return range.load (std::memory_order_relaxed); }
    float maxMomentaryLufs() const noexcept { return maxMomentary.load (std::memory_order_relaxed); }
    float maxShortTermLufs() const noexcept { return maxShortTerm.load (std::memory_order_relaxed); }

    static double toLufs (double meanSquare) noexcept { return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare) : -1.0e9; }

private:
    static int binOf (double lufs) noexcept { return std::clamp ((int) std::floor ((lufs - kFloor) / kBin), 0, kBins - 1); }

    double meanOfLast (int k) const noexcept
    {
        double e = 0.0;
        for (int j = 0; j < k; ++j)
            e += sub[(size_t) ((subPos - 1 - j + 64) % 64)];
        return e / k;
    }

    void endSubBlock() noexcept
    {
        sub[(size_t) subPos] = acc / (double) accN;
        subPos = (subPos + 1) % 64;
        subFilled = std::min (subFilled + 1, 64);
        acc = 0.0;
        accN = 0;

        if (subFilled >= 4)
        {
            const double ms = meanOfLast (4);
            const double l = toLufs (ms);
            momentary.store ((float) l, std::memory_order_relaxed);
            if (l > maxMomentary.load (std::memory_order_relaxed))
                maxMomentary.store ((float) l, std::memory_order_relaxed);
            if (l > kFloor) // absolute gate
            {
                const int b = binOf (l);
                ++intCount[(size_t) b];
                intEnergy[(size_t) b] += ms;
                ++intTotal;
                updateIntegrated();
            }
        }
        if (subFilled >= 30)
        {
            const double ms = meanOfLast (30);
            const double l = toLufs (ms);
            shortTerm.store ((float) l, std::memory_order_relaxed);
            if (l > maxShortTerm.load (std::memory_order_relaxed))
                maxShortTerm.store ((float) l, std::memory_order_relaxed);
            if (l > kFloor)
            {
                const int b = binOf (l);
                ++lraCount[(size_t) b];
                lraEnergy[(size_t) b] += ms;
                ++lraTotal;
                updateRange();
            }
        }
    }

    // energy mean of the gated blocks above a threshold (bins from the threshold's bin upwards; the
    // threshold's own bin is included in proportion to the part above the threshold)
    static double gatedMean (const std::vector<long>& cnt, const std::vector<double>& en, double thr, long& n) noexcept
    {
        const int b0 = binOf (thr);
        double e = 0.0;
        double nn = 0.0;
        const double frac = 1.0 - std::clamp ((thr - (kFloor + b0 * kBin)) / kBin, 0.0, 1.0);
        e += en[(size_t) b0] * frac;
        nn += (double) cnt[(size_t) b0] * frac;
        for (int b = b0 + 1; b < kBins; ++b)
        {
            e += en[(size_t) b];
            nn += (double) cnt[(size_t) b];
        }
        n = (long) std::lround (nn);
        return nn > 0.0 ? e / nn : 0.0;
    }

    void updateIntegrated() noexcept
    {
        long n = 0;
        const double ungated = gatedMean (intCount, intEnergy, kFloor, n);
        if (n <= 0)
            return;
        const double rel = toLufs (ungated) - 10.0;
        const double gated = gatedMean (intCount, intEnergy, std::max (rel, kFloor), n);
        if (n > 0)
            integrated.store ((float) toLufs (gated), std::memory_order_relaxed);
    }

    void updateRange() noexcept
    {
        long n = 0;
        const double mean = gatedMean (lraCount, lraEnergy, kFloor, n);
        if (n <= 0)
            return;
        const double rel = std::max (toLufs (mean) - 20.0, kFloor);
        const int b0 = binOf (rel);
        long total = 0;
        for (int b = b0; b < kBins; ++b)
            total += lraCount[(size_t) b];
        if (total < 2)
        {
            range.store (0.0f, std::memory_order_relaxed);
            return;
        }
        // percentiles on the bin centres (nearest-rank, 0.01 LU resolution)
        auto percentile = [&] (double p)
        {
            const long target = std::max (1L, (long) std::ceil (p * (double) total));
            long c = 0;
            for (int b = b0; b < kBins; ++b)
            {
                c += lraCount[(size_t) b];
                if (c >= target)
                    return kFloor + (b + 0.5) * kBin;
            }
            return kTop;
        };
        range.store ((float) (percentile (0.95) - percentile (0.10)), std::memory_order_relaxed);
    }

    double fs = 48000.0;
    int nCh = 2, subLen = 4800, accN = 0, subPos = 0, subFilled = 0;
    double acc = 0.0;
    KWeighting kw;
    std::array<double, 64> sub {};
    std::vector<long> intCount, lraCount;
    std::vector<double> intEnergy, lraEnergy;
    long intTotal = 0, lraTotal = 0;
    std::atomic<float> momentary { kNone }, shortTerm { kNone }, integrated { kNone }, range { 0.0f },
        maxMomentary { kNone }, maxShortTerm { kNone };
};
} // namespace axell
