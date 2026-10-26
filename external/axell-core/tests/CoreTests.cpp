// Axell core - DSP tests (plain g++, no JUCE). Exit code = number of failed checks.
//   loudness: EBU Tech 3341 (cases 1-5, stereo) and EBU Tech 3342 (LRA cases 1-4) conformance, at 44.1 / 48 / 96 kHz
//   true peak: sines of known amplitude, any phase, up to 0.45 fs
//   dither: grid, TPDF noise level, shaped spectrum; limiter: ceiling; handshake: request / acknowledge / timeout
//   sibilance: Velvet 1.0 (default), 3.0 and 3.0 LIVE networks on s / sh-shaped noise and a voiced tone, 44.1-192 kHz
#include "../include/axell/dsp/Dither.h"
#include "../include/axell/dsp/LatencyHandshake.h"
#include "../include/axell/dsp/MasterLimiter.h"
#include "../include/axell/dsp/Loudness.h"
#include "../include/axell/dsp/TruePeak.h"
#include "../include/axell/dsp/TruePeakLimiter.h"

#include <axell/dsp/SibilanceDetector.h>
#include <axell/dsp/VoicePresence.h>
#include <axell/dsp/MinPhase.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace axell;
static int fails = 0, checks = 0;
static void check (const std::string& name, double v, double lo, double hi, const char* unit)
{
    ++checks;
    const bool ok = v >= lo && v <= hi;
    if (! ok)
        ++fails;
    std::printf ("%s  %-86s %10.4f %s  [%g .. %g]\n", ok ? "PASS" : "FAIL", name.c_str(), v, unit, lo, hi);
}

// stereo 1 kHz sine segments: {dBFS peak, seconds}
static std::vector<float> tones (double fs, std::initializer_list<std::pair<double, double>> segs)
{
    std::vector<float> x;
    double t = 0.0;
    for (auto [dbfs, secs] : segs)
    {
        const double a = std::pow (10.0, dbfs / 20.0);
        const long n = (long) std::llround (secs * fs);
        for (long i = 0; i < n; ++i, t += 1.0 / fs)
            x.push_back ((float) (a * std::sin (2.0 * kPi * 1000.0 * t)));
    }
    return x;
}

static void measure (LoudnessMeter& m, const std::vector<float>& x, double fs)
{
    m.prepare (fs, 2);
    const int B = 512;
    for (size_t p = 0; p < x.size(); p += B)
    {
        const int n = (int) std::min<size_t> (B, x.size() - p);
        const float* ch[2] = { x.data() + p, x.data() + p };
        m.process (ch, 2, n);
    }
}

int main()
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const std::string r = " @" + std::to_string ((int) fs);
        LoudnessMeter m;
        // EBU 3341 case 1 / 2: steady tones (momentary, short-term and integrated)
        measure (m, tones (fs, { { -23, 20 } }), fs);
        check ("3341-1: -23 dBFS stereo 1 kHz, integrated" + r, m.integratedLufs(), -23.1, -22.9, "LUFS");
        check ("3341-1: momentary" + r, m.momentaryLufs(), -23.1, -22.9, "LUFS");
        check ("3341-1: short-term" + r, m.shortTermLufs(), -23.1, -22.9, "LUFS");
        measure (m, tones (fs, { { -33, 20 } }), fs);
        check ("3341-2: -33 dBFS, integrated" + r, m.integratedLufs(), -33.1, -32.9, "LUFS");
        // case 3 / 4 / 5: relative gate
        measure (m, tones (fs, { { -36, 10 }, { -23, 60 }, { -36, 10 } }), fs);
        check ("3341-3: -36/-23/-36 dBFS (10/60/10 s), integrated" + r, m.integratedLufs(), -23.1, -22.9, "LUFS");
        measure (m, tones (fs, { { -72, 10 }, { -36, 10 }, { -23, 60 }, { -36, 10 }, { -72, 10 } }), fs);
        check ("3341-4: -72/-36/-23/-36/-72 dBFS, integrated" + r, m.integratedLufs(), -23.1, -22.9, "LUFS");
        measure (m, tones (fs, { { -26, 20 }, { -20, 20.1 }, { -26, 20 } }), fs);
        check ("3341-5: -26/-20/-26 dBFS (20/20.1/20 s), integrated" + r, m.integratedLufs(), -23.1, -22.9, "LUFS");
        // EBU 3342 LRA
        measure (m, tones (fs, { { -20, 20 }, { -30, 20 } }), fs);
        check ("3342-1: -20/-30 dBFS, LRA" + r, m.loudnessRangeLu(), 9, 11, "LU");
        measure (m, tones (fs, { { -20, 20 }, { -15, 20 } }), fs);
        check ("3342-2: -20/-15 dBFS, LRA" + r, m.loudnessRangeLu(), 4, 6, "LU");
        measure (m, tones (fs, { { -40, 20 }, { -20, 20 } }), fs);
        check ("3342-3: -40/-20 dBFS, LRA" + r, m.loudnessRangeLu(), 19, 21, "LU");
        measure (m, tones (fs, { { -50, 20 }, { -35, 20 }, { -20, 20 }, { -35, 20 }, { -50, 20 } }), fs);
        check ("3342-4: -50/-35/-20/-35/-50 dBFS, LRA" + r, m.loudnessRangeLu(), 14, 16, "LU");

        // true peak: sines of amplitude 0.5, random phase and frequency up to 0.45 fs
        TruePeakMeter tp;
        tp.prepare();
        std::mt19937 rng (7);
        double worstUnder = 0, worstOver = 0;
        for (int k = 0; k < 40; ++k)
        {
            const double f = std::uniform_real_distribution<double> (20.0, 0.45 * fs) (rng);
            const double ph = std::uniform_real_distribution<double> (0.0, 6.283) (rng);
            std::vector<float> x ((size_t) (0.2 * fs));
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = (float) (0.5 * std::sin (2.0 * kPi * f * (double) i / fs + ph));
            // measure after the first 50 ms: an abrupt onset really overshoots in the ideal reconstruction (Gibbs)
            tp.reset();
            const size_t skip = (size_t) (0.05 * fs);
            const float* ch0[1] = { x.data() };
            tp.process (ch0, 1, (int) skip);
            tp.clearPeak();
            const float* ch[1] = { x.data() + skip };
            tp.process (ch, 1, (int) (x.size() - skip));
            const double e = 20.0 * std::log10 (tp.maxTruePeak() / 0.5);
            worstUnder = std::min (worstUnder, e);
            worstOver = std::max (worstOver, e);
        }
        check ("true peak, sines up to 0.45 fs, any phase: worst under-read" + r, worstUnder, -0.05, 0.0, "dB");
        check ("true peak, same: worst over-read" + r, worstOver, 0.0, 0.05, "dB");

        // limiter ceiling (band-limited multitone driven +12 dB)
        TruePeakLimiter lim;
        lim.prepare (fs, (int) (0.005 * fs));
        lim.setLookahead ((int) (0.0015 * fs));
        lim.resetState();
        lim.setParams (-1.0f, 60.0f);
        std::vector<float> y ((size_t) fs);
        for (size_t i = 0; i < y.size(); ++i)
        {
            const double t = (double) i / fs;
            y[i] = (float) (1.2 * (std::sin (2 * kPi * 110 * t) + 0.7 * std::sin (2 * kPi * 3170 * t + 1) + 0.5 * std::sin (2 * kPi * 0.4 * fs * t)));
        }
        float* io[1] = { y.data() };
        lim.process (io, 1, (int) y.size());
        float sp = 0;
        for (size_t i = (size_t) (0.1 * fs); i < y.size(); ++i)
            sp = std::max (sp, std::abs (y[i]));
        check ("limiter -1 dBTP, multitone +12 dB: sample peak" + r, 20.0 * std::log10 (sp), -60.0, -1.0, "dBFS");
        TruePeakMeter tpo;
        tpo.prepare();
        const float* oc[1] = { y.data() + (size_t) (0.1 * fs) };
        tpo.process (oc, 1, (int) (y.size() - (size_t) (0.1 * fs)));
        check ("limiter -1 dBTP, multitone +12 dB: true peak (meter)" + r, 20.0 * std::log10 (tpo.maxTruePeak()), -60.0, -1.0, "dBTP");
    }

    // mastering limiter: ceiling per style and link, transparency, latency, program-dependent release
    for (double fs : { 44100.0, 48000.0 })
    {
        const std::string r = " @" + std::to_string ((int) fs);
        auto drums = [&] (double secs)
        {
            std::vector<float> L ((size_t) (secs * fs)), R (L.size());
            std::mt19937 g (3);
            std::normal_distribution<double> nd;
            for (size_t i = 0; i < L.size(); ++i)
            {
                const double t = (double) i / fs, beat = std::fmod (t, 0.5);
                const double kick = std::exp (-beat * 14) * std::sin (2 * kPi * (50 + 80 * std::exp (-beat * 35)) * beat);
                const double sn = std::exp (-std::fmod (t + 0.25, 0.5) * 25) * nd (g) * 0.4;
                const double pad = 0.25 * std::sin (2 * kPi * 220 * t) + 0.2 * std::sin (2 * kPi * 330 * t + 1);
                L[i] = (float) (1.4 * (kick + sn + pad));
                R[i] = (float) (1.4 * (kick + 0.8 * sn + 0.9 * pad));
            }
            // band-limit to 0.47 fs (the guaranteed band) with a long windowed sinc
            std::vector<float>* chs[2] = { &L, &R };
            const int H = 200;
            std::vector<double> h (2 * H + 1);
            for (int k = -H; k <= H; ++k)
                h[(size_t) (k + H)] = (k == 0 ? 0.94 : std::sin (kPi * 0.94 * k) / (kPi * k)) * (0.42 + 0.5 * std::cos (kPi * k / H) + 0.08 * std::cos (2 * kPi * k / H));
            for (auto* chp : chs)
            {
                std::vector<float> o (chp->size());
                for (long i = 0; i < (long) o.size(); ++i)
                {
                    double acc = 0;
                    for (int k = -H; k <= H; ++k)
                        if (i - k >= 0 && i - k < (long) o.size())
                            acc += (*chp)[(size_t) (i - k)] * h[(size_t) (k + H)];
                    o[(size_t) i] = (float) acc;
                }
                *chp = o;
            }
            return std::make_pair (L, R);
        };
        const auto [L0, R0] = drums (4.0);
        const char* names[] = { "transparent", "punchy", "loud", "safe" };
        for (int st = 0; st < 4; ++st)
            for (float lk : { 1.0f, 0.0f })
            {
                MasterLimiter ml;
                ml.prepare (fs, (int) (0.005 * fs));
                ml.setLookahead ((int) (0.002 * fs));
                ml.reset();
                ml.setParams (-1.0f, 120.0f, (MasterLimiter::Style) st, lk);
                auto L = L0, R = R0;
                float* io[2] = { L.data(), R.data() };
                ml.process (io, 2, (int) L.size());
                TruePeakMeter m;
                m.prepare();
                const size_t from = (size_t) (0.5 * fs);
                const float* oc[2] = { L.data() + from, R.data() + from };
                m.process (oc, 2, (int) (L.size() - from));
                check (std::string ("master limiter ") + names[st] + ", link " + (lk > 0.5f ? "100" : "0") + " %, drums +~10 dB: true peak" + r,
                       20 * std::log10 (m.maxTruePeak()), -60, -1.0, "dBTP");
            }
        // below the ceiling: exact delay
        MasterLimiter ml;
        ml.prepare (fs, (int) (0.005 * fs));
        ml.setLookahead ((int) (0.002 * fs));
        ml.reset();
        ml.setParams (-1.0f, 120.0f, MasterLimiter::Style::transparent, 1.0f);
        std::vector<float> q (L0.size()), q2;
        for (size_t i = 0; i < q.size(); ++i)
            q[i] = L0[i] * 0.2f;
        q2 = q;
        float* io[1] = { q2.data() };
        ml.process (io, 1, (int) q2.size());
        double e = 0;
        const int Lat = ml.latency();
        for (size_t i = (size_t) Lat; i < q.size(); ++i)
            e = std::max (e, (double) std::abs (q2[i] - q[i - (size_t) Lat]));
        check ("master limiter below the ceiling: output = input delayed by latency()" + r, e, 0, 0, "");
        // VOICE HOLD (pause flags): a loud tone (limited ~9 dB), then a quiet one flagged as a pause -> the gain stays at
        // the voice's; a loud burst inside the "pause" still never passes the ceiling
        {
            MasterLimiter vh;
            vh.prepare (fs, (int) (0.005 * fs));
            vh.setLookahead ((int) (0.002 * fs));
            vh.reset();
            vh.setParams (-1.0f, 120.0f, MasterLimiter::Style::transparent, 1.0f);
            const size_t n1 = (size_t) fs, n2 = (size_t) (0.6 * fs), nb = (size_t) (0.05 * fs);
            std::vector<float> x (n1 + n2 + nb + n2);
            std::vector<uint8_t> pz (x.size(), 0);
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double amp = i < n1 ? 2.5 : (i >= n1 + n2 && i < n1 + n2 + nb ? 4.0 : 0.01);
                x[i] = (float) (amp * std::sin (2 * 3.141592653589793 * 997.0 * i / fs));
                pz[i] = i >= n1 ? 1 : 0;
            }
            float* vio[1] = { x.data() };
            float grHeld = 0.0f;
            for (size_t pos = 0; pos < x.size(); pos += 256)
            {
                float* b[1] = { x.data() + pos };
                const int m = (int) std::min<size_t> (256, x.size() - pos);
                vh.process (b, 1, m, pz.data() + pos);
                if (pos <= n1 + (size_t) (0.4 * fs) && pos + 256 > n1 + (size_t) (0.4 * fs))
                    grHeld = vh.gainReductionDb (0);
            }
            (void) vio;
            double tp = 0.0;
            for (float v : x)
                tp = std::max (tp, (double) std::abs (v));
            check ("master limiter VOICE HOLD: gain reduction 0.4 s into a pause (voice ~8.9 dB)" + r, grHeld, 7.5, 9.5, "dB");
            check ("master limiter VOICE HOLD: a burst inside the pause stays under the ceiling (sample peak)" + r,
                   20 * std::log10 (tp), -60, -1.0, "dBFS");
        }
        check ("master limiter latency = 64 + lookahead" + r, Lat, 64 + (int) (0.002 * fs), 64 + (int) (0.002 * fs), "samples");
        // program dependence: after 2 s of sustained limiting the release is slower than after one short peak
        auto recover = [&] (double holdSecs)
        {
            MasterLimiter m2;
            m2.prepare (fs, (int) (0.005 * fs));
            m2.setLookahead ((int) (0.002 * fs));
            m2.reset();
            m2.setParams (-1.0f, 100.0f, MasterLimiter::Style::transparent, 1.0f);
            std::vector<float> x ((size_t) ((holdSecs + 1.0) * fs));
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = (float) ((i < (size_t) (holdSecs * fs) ? 2.0 : 0.3) * std::sin (2 * kPi * 1000 * (double) i / fs));
            float* p2[1] = { x.data() };
            m2.process (p2, 1, (int) x.size());
            // time until the gain is within 1 dB of unity after the loud part
            const size_t end = (size_t) (holdSecs * fs) + (size_t) m2.latency();
            for (size_t i = end; i < x.size(); ++i)
                if (std::abs (x[i]) > 0.0f && m2.gainReductionDb (0) >= 0.0f)
                {
                    // measure from the output envelope: peak of each 1 ms vs 0.3
                    size_t j = i;
                    while (j + 48 < x.size())
                    {
                        float pk = 0;
                        for (size_t k = j; k < j + (size_t) (0.001 * fs); ++k)
                            pk = std::max (pk, std::abs (x[k]));
                        if (pk > 0.3f * 0.891f)
                            return (double) (j - end) / fs * 1000.0;
                        j += (size_t) (0.001 * fs);
                    }
                    break;
                }
            return 1e9;
        };
        const double shortRec = recover (0.02), longRec = recover (2.0);
        check ("master limiter transparent: recovery after 20 ms peak (to within 1 dB)" + r, shortRec, 0, 400, "ms");
        check ("master limiter transparent: recovery after 2 s of limiting is slower (ratio)" + r, longRec / std::max (shortRec, 1e-3), 1.5, 50, "x");
    }

    // dither: on the 16-bit grid, TPDF error RMS = LSB / 2 (-96.3 dBFS), shaped: less error below 2 kHz
    {
        const double fs = 48000.0, lsb = 1.0 / 32768.0;
        std::vector<float> a ((size_t) fs), b;
        for (size_t i = 0; i < a.size(); ++i)
            a[i] = (float) (0.25 * std::sin (2 * kPi * 440 * (double) i / fs));
        b = a;
        Dither d;
        d.set (16, Dither::Mode::tpdf);
        float* io[1] = { b.data() };
        d.process (io, 1, (int) b.size());
        double off = 0, e2 = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            off = std::max (off, std::abs (b[i] / lsb - std::round (b[i] / lsb)));
            e2 += (double) (b[i] - a[i]) * (b[i] - a[i]);
        }
        check ("dither 16-bit TPDF: distance from the grid (LSB)", off, 0, 1e-3, "LSB");
        check ("dither 16-bit TPDF: error RMS", 10 * std::log10 (e2 / (double) a.size()), -97.0, -95.5, "dBFS");
        // shaped: error low-passed by a 32-tap moving average (~ < 1.5 kHz) is quieter than TPDF's
        std::vector<float> c = a;
        Dither s;
        s.prepare (fs);
        s.set (16, Dither::Mode::shaped);
        float* io2[1] = { c.data() };
        s.process (io2, 1, (int) c.size());
        // error power in a band: Hann-windowed DFT bins (window sidelobes far below the shaped noise's +25 dB at 20 kHz)
        auto bandDb = [&] (const std::vector<float>& y, double lo, double hi)
        {
            const int N = 8192, from = 8000;
            const double df = fs / N;
            double sum = 0;
            int bins = 0;
            for (int k = (int) std::ceil (lo / df); k <= (int) (hi / df); ++k, ++bins)
            {
                double re = 0, im = 0;
                for (int i = 0; i < N; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos (2 * kPi * i / N), x = ((double) y[(size_t) (from + i)] - a[(size_t) (from + i)]) * w;
                    re += x * std::cos (2 * kPi * k * i / N);
                    im -= x * std::sin (2 * kPi * k * i / N);
                }
                sum += re * re + im * im;
            }
            return 10 * std::log10 (sum / bins);
        };
        check ("dither F-weighted 9th order vs TPDF: error below 1.5 kHz (difference)", bandDb (c, 20, 1500) - bandDb (b, 20, 1500), -60, -10, "dB");
        check ("dither F-weighted 9th order vs TPDF: error at 2-5 kHz, most sensitive band (difference)", bandDb (c, 2000, 5000) - bandDb (b, 2000, 5000), -60, -18, "dB");
        double peakErr = 0;
        for (size_t i = 0; i < c.size(); ++i)
            peakErr = std::max (peakErr, std::abs ((double) c[i] - a[i]) / lsb);
        check ("dither F-weighted: peak error (stable loop)", peakErr, 0, 40, "LSB");
    }

    // handshake
    {
        LatencyHandshake h;
        h.prepared (100, 48000.0);
        check ("handshake: without a host the new latency is ready at once", h.ready (200, true, 512) ? 1 : 0, 1, 1, "");
        h.prepared (100, 48000.0);
        h.setHostLatency (100);
        check ("handshake: tracking, new latency not ready before the host was told", h.ready (200, true, 512) ? 1 : 0, 0, 0, "");
        int lat = 0;
        check ("handshake: the request carries the new latency", h.take (lat) ? lat : -1, 200, 200, "samples");
        h.ack();
        check ("handshake: ready after the acknowledgement", h.ready (200, true, 512) ? 1 : 0, 1, 1, "");
        h.ready (300, true, 512);
        bool r = false;
        for (int i = 0; i < 60; ++i)
            r = h.ready (300, true, 512);
        check ("handshake: unanswered for 0.5 s -> withdrawn, holding the old one", (! r && h.isHolding()) ? 1 : 0, 1, 1, "");
        check ("handshake: a withdrawn request cannot be taken", h.take (lat) ? 1 : 0, 0, 0, "");
    }

    // sibilance detector: Velvet 1.0's network by default (Axell Gate 2.0 was trained on its output), Velvet 3.0's and its
    // causal LIVE network on request; every network finds noise bursts and leaves a voiced tone alone, at every rate
    for (int kind = 0; kind < 3; ++kind)
        for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const auto cfg = kind == 0 ? voice::DetectorConfig {} : kind == 1 ? voice::DetectorConfig::velvet3() : voice::DetectorConfig::live();
            voice::SibilanceDetector d;
            d.prepare (fs, cfg);
            std::mt19937 g (7);
            std::normal_distribution<double> nd;
            // sibilant-like noise: white noise through two resonances, alternately "s" (7.5 kHz) and "sh" (3.4 kHz), the
            // shapes real sibilants have (a flat hiss is not one: Velvet 3.0's network rightly gives it p ~ 0.35); the
            // band power is kept the same at every rate
            auto bandPass = [fs] (Biquad& b, double f, double q)
            {
                const double w = 2.0 * 3.141592653589793 * f / fs, al = std::sin (w) / (2.0 * q), a0 = 1.0 + al;
                b.setRaw (al / a0, 0.0, -al / a0, -2.0 * std::cos (w) / a0, (1.0 - al) / a0);
            };
            Biquad s1, s2, h1, h2;
            bandPass (s1, 7500.0, 2.5);
            bandPass (s2, 7500.0, 2.5);
            bandPass (h1, 3400.0, 1.5);
            bandPass (h2, 3400.0, 1.5);
            const double norm = std::sqrt (fs / 48000.0);
            double ph = 0.0, ps = 0, pv = 0;
            int ns = 0, nv = 0;
            const size_t seg = (size_t) (0.3 * fs);
            for (size_t i = 0; i < (size_t) (6.0 * fs); ++i)
            {
                const bool noise = (i / seg) % 2 == 1;
                ph += 2.0 * 3.141592653589793 * 140.0 / fs;
                double v = 0.0;
                for (int h = 1; h < 20; ++h)
                    v += std::sin (h * ph) / h * std::exp (-std::pow ((h * 140.0 - 700.0) / 600.0, 2));
                const double w0 = nd (g);
                const double n = norm * ((i / seg) % 4 == 1 ? s2.process (s1.process (w0, 0), 0) : h2.process (h1.process (w0, 0), 0));
                const float x = (float) (noise ? 0.6 * n : 0.2 * v);
                if (d.push (x) && (size_t) d.out().center > (size_t) fs)
                {
                    const size_t c = (size_t) d.out().center, u = c % seg;
                    if (u > seg / 5 && u < seg - seg / 5)
                    {
                        if ((c / seg) % 2 == 1) { ps += d.out().p; ++ns; }
                        else { pv += d.out().p; ++nv; }
                    }
                }
            }
            const std::string r = std::string (kind == 0 ? " [Velvet 1.0]" : kind == 1 ? " [Velvet 3.0]" : " [Velvet 3.0 LIVE]") + " @ "
                                  + std::to_string ((int) fs);
            check ("sibilance: mean probability on s / sh-shaped noise" + r, ps / ns, 0.6, 1.0, "");
            check ("sibilance: mean probability on a voiced tone" + r, pv / nv, 0.0, 0.05, "");
            if (kind == 2)
                check ("sibilance LIVE: frames reported at once (no smoothing delay)" + r, (double) d.frameDelay(), d.windowSize() / 2, d.windowSize() / 2, "smp");
        }


    // voice decision (Axell Gate 2.0's, shared): a synthetic voice - vowels through formants, 1.6 s phrases, 0.8 s pauses
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        axell::voice::VoicePresence vp;
        vp.prepare (fs);
        const std::string r = " @ " + std::to_string ((int) fs);
        std::mt19937 g (5);
        std::normal_distribution<double> nd;
        const double pi = 3.14159265358979323846;
        double ph = 0, b0 = 0, inV = 0, nV = 0, inP = 0, nP = 0;
        const size_t n = (size_t) (9.6 * fs);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / fs, phrase = std::fmod (t, 2.4), syl = std::fmod (phrase, 0.32);
            double y = 0;
            if (phrase > 0.2 && phrase < 1.8 && syl < 0.22)
            {
                const double f0 = 140.0 + 25.0 * std::sin (2 * pi * 0.6 * t), env = std::min (1.0, std::min (syl, 0.22 - syl) / 0.015);
                ph += 2 * pi * f0 / fs;
                for (int h = 1; h * f0 < 8000.0; ++h)
                {
                    const double f = h * f0;
                    y += (std::exp (-std::pow ((f - 650) / 250, 2)) + 0.6 * std::exp (-std::pow ((f - 1150) / 300, 2))
                          + 0.2 * std::exp (-std::pow ((f - 2600) / 500, 2))) * std::sin (h * ph) / std::sqrt ((double) h);
                }
                y *= 0.35 * env;
            }
            b0 = 0.997 * b0 + 0.03 * nd (g);
            vp.push ((float) (0.316 * y + 0.0016 * b0));
            if (t > 2.4 && phrase > 0.3 && phrase < 1.7)
                inV += vp.voice() ? 1 : 0, ++nV;
            if (t > 2.4 && (phrase > 1.9 && phrase < 2.35))
                inP += vp.voice() ? 1 : 0, ++nP;
        }
        check ("voice presence: share of the phrases called voice (their 0.1 s consonant gaps are silent here)" + r, inV / nV, 0.7, 1.0, "");
        check ("voice presence: share of the pauses called voice" + r, inP / nP, 0.0, 0.1, "");
        check ("voice presence: decision delay" + r, 1000.0 * vp.frameDelay() / fs, 0.0, 15.0, "ms");
        axell::voice::VoicePresence q;
        q.prepare (fs);
        bool any = false;
        for (int i = 0; i < (int) fs; ++i)
        {
            q.push (0.0f);
            any = any || q.voice();
        }
        check ("voice presence: silence is never voice" + r, any ? 1.0 : 0.0, 0.0, 0.0, "");
    }


    // minimum-phase filter from a magnitude (Velvet 3.0 LIVE's method, shared)
    {
        axell::MinPhaseDesigner d;
        d.prepare (2048, 512);
        std::vector<float> mag ((size_t) d.bins(), 1.0f), h (512);
        d.design (mag.data(), h.data());
        double off = std::abs (h[0] - 1.0);
        for (int i = 1; i < 512; ++i)
            off = std::max (off, (double) std::abs (h[(size_t) i]));
        check ("min-phase: flat magnitude -> identity (max error)", off, 0.0, 1.0e-4, "");
        // a de-reverb-like magnitude: -12 dB below 300 Hz and above 6 kHz (48 kHz grid), smooth edges
        for (int k = 0; k < d.bins(); ++k)
        {
            const double f = 24000.0 * k / (d.bins() - 1);
            const double lo = 1.0 / (1.0 + std::exp ((300.0 - f) / 40.0)), hi = 1.0 / (1.0 + std::exp ((f - 6000.0) / 400.0));
            mag[(size_t) k] = (float) (0.25 + 0.75 * lo * hi);
        }
        d.design (mag.data(), h.data());
        double worst = 0.0, eFront = 0.0, eAll = 0.0;
        for (double f : { 100.0, 1000.0, 3000.0, 12000.0 })
        {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < 512; ++i)
            {
                re += h[(size_t) i] * std::cos (2 * 3.14159265358979323846 * f * i / 48000.0);
                im -= h[(size_t) i] * std::sin (2 * 3.14159265358979323846 * f * i / 48000.0);
            }
            const int k = (int) std::lround (f / 24000.0 * (d.bins() - 1));
            worst = std::max (worst, std::abs (20.0 * std::log10 (std::hypot (re, im) / mag[(size_t) k])));
        }
        for (int i = 0; i < 512; ++i)
        {
            eAll += (double) h[(size_t) i] * h[(size_t) i];
            if (i < 64)
                eFront += (double) h[(size_t) i] * h[(size_t) i];
        }
        check ("min-phase: magnitude at 100 Hz / 1 / 3 / 12 kHz vs the design (worst)", worst, 0.0, 0.3, "dB");
        check ("min-phase: share of the energy in the first 1.3 ms", eFront / eAll, 0.95, 1.0, "");
        // the time-varying FIR: identity in, identity out; a new filter fades in without a step
        axell::MinPhaseFir fir;
        fir.reset();                                           // before prepare(): must be harmless
        fir.prepare (512, 480);
        double e1 = 0.0;
        for (int i = 0; i < 2000; ++i)
        {
            const float x = (float) std::sin (0.05 * i);
            e1 = std::max (e1, (double) std::abs (fir.process (x) - x));
        }
        check ("min-phase FIR: identity before the first filter (max error)", e1, 0.0, 1.0e-6, "");
        fir.setFilter (h.data());
        std::vector<float> xin (4000), yout (4000);
        for (int i = 0; i < 4000; ++i)
            xin[(size_t) i] = (float) std::sin (0.031 * i) * (i % 7 == 0 ? 1.0f : 0.3f);
        for (int i = 0; i < 4000; ++i)
            yout[(size_t) i] = fir.process (xin[(size_t) i]);
        double e2 = 0.0;
        for (int i = 2000; i < 4000; ++i)                      // after the fade: the filter exactly
        {
            double ref = 0.0;
            for (int k = 0; k < 512; ++k)
                ref += (double) h[(size_t) k] * xin[(size_t) (i - k)];
            e2 = std::max (e2, std::abs (ref - yout[(size_t) i]));
        }
        check ("min-phase FIR: after the fade = direct convolution (max error)", e2, 0.0, 1.0e-4, "");
    }

    std::printf ("%s: %d of %d checks passed\n", fails == 0 ? "ALL PASS" : "FAILURES", checks - fails, checks);
    return fails;
}
