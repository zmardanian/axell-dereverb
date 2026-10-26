#pragma once
// Axell core - sibilance detector (from Axell Velvet, identical numerics: 1.0's features, 3.0's network choice and
// causal mode). Every 2 ms hop, on a mono signal: a
// 12 ms Hann STFT and a YIN aperiodicity measure (de Cheveigne & Kawahara, JASA 2002) give ten features (aperiodicity,
// HF flatness, level against the voice's own loud level, six band levels and the 1.5-14 kHz total against the voice
// body), adapted to the singer and smoothed over +-2 hops; with five of them averaged over the past 48 ms they feed a
// 19 -> 32 -> 1 network that gives the probability of a sibilant. Networks: Velvet 1.0's (SibilanceModel.h, the default:
// Axell Gate 2.0 was trained on its output), Velvet 3.0's (SibilanceModel3.h, 111 voices; DetectorConfig::network = 3)
// and Velvet 3.0's LIVE network for the causal mode (SibilanceModel3Live.h, DetectorConfig::live()). The frames also carry the raw
// features, band energies, body / frame level, f0 and aperiodicity for other detectors (Axell Gate's voice activity).
// Real-time safe: prepare() allocates; push() never allocates, locks or throws.
#include <axell/dsp/Fft.h>
#include <axell/dsp/Filters.h>
#include <axell/dsp/Math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "SibilanceModel.h"
#include "SibilanceModel3.h"
#include "SibilanceModel3Live.h"

namespace axell::voice
{

constexpr int kNumFeat = 10;   // ap, flatness, level re loud, 6 bands re body, 1.5-14 kHz re body
constexpr int kNumBase = 14;               // per-frame model inputs
constexpr int kNumCtx = 5;                 // inputs also averaged over the past kCtxFrames
constexpr int kCtxIdx[kNumCtx] = { 0, 1, 9, 10, 11 };
constexpr int kCtxFrames = 24;
constexpr int kNumDesign = kNumBase + kNumCtx;
static_assert (kNumDesign == model::kInputs && kNumDesign == model3::kInputs && kNumDesign == model3_live::kInputs,
               "a sibilance network does not match the detector");

struct ModelRef
{
    int hidden;
    const float *mu, *sd, *w1, *b1, *w2;
    float b2;
};
inline constexpr ModelRef kVelvet1Model { model::kHidden, model::kMu, model::kSd, model::kW1, model::kB1, model::kW2, model::kB2 };
inline constexpr ModelRef kVelvet3Model { model3::kHidden, model3::kMu, model3::kSd, model3::kW1, model3::kB1, model3::kW2, model3::kB2 };
inline constexpr ModelRef kVelvet3LiveModel { model3_live::kHidden, model3_live::kMu, model3_live::kSd, model3_live::kW1,
                                              model3_live::kB1, model3_live::kW2, model3_live::kB2 };
constexpr int kNumBands = 10;  // de-esser bands
constexpr double kEdges[kNumBands + 1] = { 2000, 2800, 3600, 4500, 5600, 7000, 8500, 10000, 12000, 14500, 17000 };
constexpr double kFeatBands[6][2] = { { 1500, 2500 }, { 2500, 3500 }, { 3500, 5000 }, { 5000, 7000 }, { 7000, 10000 }, { 10000, 14000 } };

struct DetectorConfig
{
    double winMs = 12.0, hopMs = 2.0;
    int smooth = 2;                    // feature smoothing +-hops (causal: the past 2 * smooth + 1 frames)
    double yinFmin = 70.0, yinFmax = 500.0, yinThresh = 0.15;
    int network = 1;                   // 1: Velvet 1.0's network (default), 3: Velvet 3.0's (111 voices)
    // causal (Velvet 3.0 LIVE): every frame as soon as its window is complete, past-only smoothing, the LIVE network
    bool causal = false;
    static DetectorConfig live() noexcept
    {
        DetectorConfig c;
        c.causal = true;
        c.smooth = 1;
        c.network = 3;
        return c;
    }
    static DetectorConfig velvet3() noexcept
    {
        DetectorConfig c;
        c.network = 3;
        return c;
    }
};

struct DetFrame
{
    int64_t center = 0;                // input sample index of the frame centre
    float feat[kNumFeat] {};           // smoothed raw features
    float adapted[kNumFeat] {};        // smoothed speaker-adapted features
    float x[kNumDesign] {};            // model inputs
    float p = 0.0f;                    // sibilance probability
    float E[kNumBands] {};             // de-esser band energies (power, unsmoothed)
    float bodyDb = -140.0f, levelDb = -140.0f, f0 = 0.0f, apRaw = 1.0f;
};

namespace detail
{
inline double besselI0 (double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < 1.0e-12 * s)
            break;
    }
    return s;
}
} // namespace detail

//==============================================================================
class SibilanceDetector
{
public:
    void prepare (double sampleRate, const DetectorConfig& c = {})
    {
        fs = sampleRate;
        cfg = c;
        net = cfg.causal ? &kVelvet3LiveModel : cfg.network == 3 ? &kVelvet3Model : &kVelvet1Model;
        hop = std::max (1, (int) std::lround (cfg.hopMs * 0.001 * fs));
        win = std::max (16, (int) std::lround (cfg.winMs * 0.001 * fs));
        nfft = 16;
        while (nfft < win)
            nfft <<= 1;
        fft.prepare (nfft);
        window.resize ((size_t) win);
        double wsum = 0.0;
        for (int i = 0; i < win; ++i)
        {
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / win));
            wsum += window[(size_t) i];
        }
        pScale = (float) (1.0 / (wsum * wsum));
        frameIn.assign ((size_t) nfft, 0.0f);
        spec.assign ((size_t) nfft + 2, 0.0f);
        power.assign ((size_t) nfft / 2 + 1, 0.0f);
        ringSize = 1;
        while (ringSize < win + 2)
            ringSize <<= 1;
        ring.assign ((size_t) ringSize, 0.0f);
        auto bin = [this] (double hz) { return std::clamp ((int) std::ceil (hz * nfft / fs), 0, nfft / 2 + 1); };
        auto range = [&] (double lo, double hi) { return std::array<int, 2> { bin (lo), std::max (bin (lo), bin (std::min (hi, 0.5 * fs))) }; };
        bodyR = range (100.0, 1500.0);
        flatR = range (2500.0, 14000.0);
        hfR = range (1500.0, 14000.0);
        for (int b = 0; b < 6; ++b)
            featR[(size_t) b] = range (kFeatBands[b][0], kFeatBands[b][1]);
        for (int b = 0; b < kNumBands; ++b)
            deR[(size_t) b] = range (kEdges[b], kEdges[b + 1]);

        // YIN: 8th-order Butterworth at 1.5 kHz, decimated to ~8 kHz
        dec = std::max (1, (int) std::lround (fs / 8000.0));
        fd = fs / dec;
        static constexpr double q[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
        for (int s = 0; s < 4; ++s)
            lp[(size_t) s].setLowPass (fs, 1500.0, q[s]);
        yW = std::max (8, (int) std::lround (cfg.winMs * 0.001 * fd));
        yTmax = (int) std::ceil (fd / cfg.yinFmin) + 1;
        yTmin = std::max (2, (int) std::floor (fd / cfg.yinFmax));
        dRingSize = 1;
        while (dRingSize < yW + yTmax + 8)
            dRingSize <<= 1;
        dRing.assign ((size_t) dRingSize, 0.0f);
        dfun.assign ((size_t) yTmax + 1, 0.0);
        dnorm.assign ((size_t) yTmax + 1, 1.0);

        adaptK = (float) (1.0 / 1500.0 * (cfg.hopMs / 2.0)); // tau 3 s
        hist.assign ((size_t) (2 * cfg.smooth + 1), Raw {});
        reset();
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (dRing.begin(), dRing.end(), 0.0f);
        for (auto& f : lp)
            f.resetState();
        n = 0;
        nd = 0;
        hopCount = 0;
        loudRef = -140.0f;
        loudInit = false;
        baseInit = false;
        base.fill (0.0f);
        histCount = 0;
        histPos = 0;
        frames = 0;
        for (auto& c : ctx)
            c.fill (0.0f);
        ctxPos = ctxCount = 0;
    }

    int hopSize() const noexcept { return hop; }
    int windowSize() const noexcept { return win; }
    int smoothing() const noexcept { return cfg.smooth; }
    double sampleRate() const noexcept { return fs; }
    // samples from the newest input to the centre of the frame push() returns
    int frameDelay() const noexcept { return win / 2 + (cfg.causal ? 0 : cfg.smooth * hop); }
    bool causal() const noexcept { return cfg.causal; }

    // feed one mono sample; true when out() holds a new frame (centre = newest - frameDelay())
    bool push (float x) noexcept
    {
        ring[(size_t) (n & (ringSize - 1))] = x;
        double y = x;
        for (auto& f : lp)
            y = f.process (y, 0);
        if (n % dec == 0)
        {
            dRing[(size_t) (nd & (dRingSize - 1))] = (float) y;
            ++nd;
        }
        ++n;
        if (++hopCount < hop)
            return false;
        hopCount = 0;
        analyse();
        return frames > (cfg.causal ? 0 : cfg.smooth);
    }

    const DetFrame& out() const noexcept { return result; }

    // a network on the design vector (default: Velvet 1.0's)
    static float probability (const float* x, const ModelRef& m = kVelvet1Model) noexcept
    {
        float z[kNumDesign];
        for (int i = 0; i < kNumDesign; ++i)
            z[i] = (x[i] - m.mu[i]) / m.sd[i];
        float o = m.b2;
        for (int j = 0; j < m.hidden; ++j)
        {
            float a = m.b1[j];
            for (int i = 0; i < kNumDesign; ++i)
                a += z[i] * m.w1[i * m.hidden + j];
            o += m.w2[j] * std::tanh (a);
        }
        o = std::clamp (o, -30.0f, 30.0f);
        return 1.0f / (1.0f + std::exp (-o));
    }

private:
    struct Raw
    {
        int64_t center = 0;
        float F[kNumFeat] {}, A[kNumFeat] {}, E[kNumBands] {};
        float bodyDb = -140.0f, levelDb = -140.0f, f0 = 0.0f;
    };

    float bandSum (const std::array<int, 2>& r) const noexcept
    {
        double s = 0.0;
        for (int k = r[0]; k < r[1]; ++k)
            s += power[(size_t) k];
        return (float) s + 1.0e-24f; // a floor far below any recording (level independence)
    }

    void yin (int64_t center, float& ap, float& f0) noexcept
    {
        ap = 1.0f;
        f0 = 0.0f;
        // decimated index of the frame centre; the window [c - W/2, c + W/2) against lags into the past
        const int64_t c = center / dec;
        const int64_t j0 = c - yW / 2, j1 = j0 + yW;
        if (j0 - yTmax < 0 || j1 > nd)
            return;
        const int mask = dRingSize - 1;
        auto at = [&] (int64_t j) { return dRing[(size_t) (j & mask)]; };
        double e0 = 0.0;
        for (int64_t j = j0; j < j1; ++j)
            e0 += (double) at (j) * at (j);
        if (e0 < 1.0e-24) // digital silence only
            return;
        dfun[0] = 0.0;
        double cum = 0.0;
        dnorm[0] = 1.0;
        for (int t = 1; t <= yTmax; ++t)
        {
            double d = 0.0;
            for (int64_t j = j0; j < j1; ++j)
            {
                const double e = (double) at (j) - at (j - t);
                d += e * e;
            }
            dfun[(size_t) t] = d;
            cum += d;
            dnorm[(size_t) t] = cum > 1.0e-30 ? d * t / cum : 1.0;
        }
        int k = -1;
        for (int t = yTmin; t < yTmax; ++t)
            if (dnorm[(size_t) t] < cfg.yinThresh)
            {
                k = t;
                while (k + 1 < yTmax && dnorm[(size_t) k + 1] < dnorm[(size_t) k])
                    ++k;
                break;
            }
        if (k < 0)
        {
            k = yTmin;
            for (int t = yTmin; t < yTmax; ++t)
                if (dnorm[(size_t) t] < dnorm[(size_t) k])
                    k = t;
        }
        double tau = k;
        if (k >= 1 && k < yTmax)
        {
            const double y0 = dnorm[(size_t) k - 1], y1 = dnorm[(size_t) k], y2 = dnorm[(size_t) k + 1];
            const double den = y0 - 2.0 * y1 + y2;
            if (den > 0.0)
                tau += std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5);
        }
        ap = (float) std::min (1.0, dnorm[(size_t) std::clamp ((int) std::lround (tau), 0, yTmax)]);
        f0 = (float) (fd / tau);
    }

    void analyse() noexcept
    {
        const int64_t newest = n - 1;
        Raw r;
        r.center = newest - win / 2;
        // STFT frame
        const int mask = ringSize - 1;
        for (int i = 0; i < win; ++i)
            frameIn[(size_t) i] = ring[(size_t) ((newest - win + 1 + i) & mask)] * window[(size_t) i];
        std::fill (frameIn.begin() + win, frameIn.end(), 0.0f);
        fft.forward (frameIn.data(), spec.data());
        double total = 0.0;
        for (int k = 0; k <= nfft / 2; ++k)
        {
            const float p = (spec[(size_t) (2 * k)] * spec[(size_t) (2 * k)] + spec[(size_t) (2 * k + 1)] * spec[(size_t) (2 * k + 1)]) * pScale;
            power[(size_t) k] = p;
            total += p;
        }
        const float body = bandSum (bodyR);
        // spectral flatness: geometric over arithmetic mean, with a floor relative to the frame (level independent)
        const int nf = std::max (1, flatR[1] - flatR[0]);
        double asum = 0.0;
        for (int k = flatR[0]; k < flatR[1]; ++k)
            asum += (double) power[(size_t) k];
        const double floorP = 1.0e-10 * asum / nf + 1.0e-30;
        double lsum = 0.0;
        asum = 0.0;
        for (int k = flatR[0]; k < flatR[1]; ++k)
        {
            lsum += std::log ((double) power[(size_t) k] + floorP);
            asum += (double) power[(size_t) k] + floorP;
        }
        const float flat = (float) (std::exp (lsum / nf) / (asum / nf));
        const float lvl = (float) (10.0 * std::log10 (total + 1.0e-24));
        // loud reference: running 95th percentile of the frame level (quantile tracker, 0.5 dB steps)
        if (lvl > -100.0f)
        {
            if (! loudInit)
            {
                loudRef = lvl;
                loudInit = true;
            }
            else
                loudRef += 0.5f * (lvl > loudRef ? 0.95f : -0.05f);
        }
        float ap, f0;
        yin (r.center, ap, f0);
        r.F[0] = ap;
        r.F[1] = flat;
        r.F[2] = lvl - (loudInit ? loudRef : lvl);
        for (int b = 0; b < 6; ++b)
            r.F[3 + b] = 10.0f * std::log10 (bandSum (featR[(size_t) b]) / body);
        r.F[9] = 10.0f * std::log10 (bandSum (hfR) / body);
        for (int b = 0; b < kNumBands; ++b)
            r.E[b] = bandSum (deR[(size_t) b]);
        r.bodyDb = 10.0f * std::log10 (body);
        r.levelDb = lvl;
        r.f0 = f0;
        // speaker adaptation (causal): band features minus their running mean over voiced, loud frames
        if (r.F[0] < 0.2f && r.F[2] > -25.0f)
        {
            if (! baseInit)
            {
                for (int i = 3; i < kNumFeat; ++i)
                    base[(size_t) i] = r.F[i];
                baseInit = true;
            }
            else
                for (int i = 3; i < kNumFeat; ++i)
                    base[(size_t) i] += adaptK * (r.F[i] - base[(size_t) i]);
        }
        for (int i = 0; i < kNumFeat; ++i)
            r.A[i] = (i >= 3 && baseInit) ? r.F[i] - base[(size_t) i] : r.F[i];
        apLast = ap;

        // history for the centred smoothing; output the frame `smooth` hops back
        const int H = (int) hist.size();
        hist[(size_t) histPos] = r;
        histPos = (histPos + 1) % H;
        histCount = std::min (histCount + 1, H);
        ++frames;
        if (! cfg.causal && frames <= cfg.smooth)
            return;
        // centred: the frame `smooth` hops back, smoothed over +-smooth; causal: the newest, smoothed over the past
        const Raw& mid = hist[(size_t) ((histPos - 1 - (cfg.causal ? 0 : cfg.smooth) + 2 * H) % H)];
        DetFrame& o = result;
        o.center = mid.center;
        for (int i = 0; i < kNumFeat; ++i)
        {
            float sf = 0.0f, sa = 0.0f;
            for (int k = 0; k < histCount; ++k)
            {
                sf += hist[(size_t) k].F[i];
                sa += hist[(size_t) k].A[i];
            }
            o.feat[i] = sf / (float) histCount;
            o.adapted[i] = sa / (float) histCount;
        }
        for (int b = 0; b < kNumBands; ++b)
            o.E[b] = mid.E[b];
        o.bodyDb = mid.bodyDb;
        o.levelDb = mid.levelDb;
        o.f0 = mid.f0;
        o.apRaw = mid.F[0];
        // design: G[:3] (smoothed raw), H[3:] (smoothed adapted), peak band, sh rise, interactions
        float* x = o.x;
        x[0] = o.feat[0];
        x[1] = o.feat[1];
        x[2] = o.feat[2];
        for (int i = 3; i < kNumFeat; ++i)
            x[i] = o.adapted[i];
        float peak = o.adapted[3];
        for (int i = 4; i < 9; ++i)
            peak = std::max (peak, o.adapted[i]);
        x[10] = peak;
        x[11] = std::max (o.adapted[4], o.adapted[5]) - o.adapted[3];
        x[12] = o.feat[0] * peak;
        x[13] = o.feat[1] * peak;
        // context: past kCtxFrames (this one included) of five inputs
        auto& slotC = ctx[(size_t) ctxPos];
        for (int i = 0; i < kNumCtx; ++i)
            slotC[(size_t) i] = x[kCtxIdx[i]];
        ctxPos = (ctxPos + 1) % kCtxFrames;
        ctxCount = std::min (ctxCount + 1, kCtxFrames);
        for (int i = 0; i < kNumCtx; ++i)
        {
            float sum = 0.0f; // summed afresh every frame: no drift over hours
            for (int k = 0; k < ctxCount; ++k)
                sum += ctx[(size_t) k][(size_t) i];
            x[kNumBase + i] = sum / (float) ctxCount;
        }
        o.p = probability (x, *net);
    }

    double fs = 48000.0, fd = 8000.0;
    DetectorConfig cfg;
    const ModelRef* net = &kVelvet1Model;
    int hop = 96, win = 576, nfft = 1024, ringSize = 1024, dec = 6, yW = 96, yTmax = 115, yTmin = 16, dRingSize = 256;
    RealFft fft;
    std::vector<float> window, frameIn, spec, power, ring, dRing;
    std::vector<double> dfun, dnorm;
    float pScale = 1.0f;
    std::array<int, 2> bodyR {}, flatR {}, hfR {};
    std::array<std::array<int, 2>, 6> featR {};
    std::array<std::array<int, 2>, kNumBands> deR {};
    std::array<Biquad, 4> lp;
    int64_t n = 0, nd = 0;
    int hopCount = 0;
    float loudRef = -140.0f, adaptK = 1.0f / 1500.0f, apLast = 1.0f;
    bool loudInit = false, baseInit = false;
    std::array<float, kNumFeat> base {};
    std::vector<Raw> hist;
    int histCount = 0, histPos = 0;
    int64_t frames = 0;
    std::array<std::array<float, kNumCtx>, kCtxFrames> ctx {};
    int ctxPos = 0, ctxCount = 0;
    DetFrame result;
};
} // namespace axell::voice
