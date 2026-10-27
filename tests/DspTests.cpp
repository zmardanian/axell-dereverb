// Axell De-Reverb - DSP test suite (JUCE-free): the engine headers exactly as they ship.
// usage: DrDspTests [--quick]   (run from the repository root: reads Resources/dereverb.bin and tests/reference)
#include "DeReverb.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <random>
#include <string>

using namespace axdr;

static int fails = 0, checks = 0;
static thread_local bool countAllocs = false;
static long allocCount = 0;
void* operator new (std::size_t n)
{
    if (countAllocs)
        ++allocCount;
    if (void* p = std::malloc (n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

static void check (const std::string& name, double v, double lo, double hi, const char* unit)
{
    ++checks;
    const bool ok = v >= lo && v <= hi;
    if (! ok)
        ++fails;
    std::printf ("%s  %-100s %12.4f %s  [%g .. %g]\n", ok ? "PASS" : "FAIL", name.c_str(), v, unit, lo, hi);
}

static std::vector<char> readFile (const char* p)
{
    std::ifstream f (p, std::ios::binary);
    return std::vector<char> ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
}
static std::vector<float> readF32 (const char* p)
{
    auto b = readFile (p);
    std::vector<float> v (b.size() / 4);
    std::memcpy (v.data(), b.data(), v.size() * 4);
    return v;
}
static double db (double e, double r) { return 10.0 * std::log10 ((e + 1e-30) / (r + 1e-30)); }

// run a DeReverb over stereo buffers in blocks (random sizes when rng given); returns the output with the latency
// removed
static std::vector<std::vector<float>> render (const Weights& W, int fs, std::vector<std::vector<float>> x, float amount,
                                               bool listen, std::mt19937* rng = nullptr, int maxBlock = 2048, bool count = false,
                                               bool ramp = false, bool trueTone = false, std::array<float, 23>* curve = nullptr)
{
    DeReverb d;
    d.prepare (W, fs, (int) x.size(), maxBlock);
    d.setTrueTone (trueTone);
    d.setAmount (ramp ? 1.0f : amount);
    d.setListen (ramp ? false : listen);
    d.reset();
    if (ramp) // as a host does it: the controls move after the start, through the smoothing
    {
        d.setAmount (amount);
        d.setListen (listen);
    }
    const int L = d.latencySamples();
    const size_t n = x[0].size();
    for (auto& c : x)
        c.resize (n + (size_t) L, 0.0f);
    std::uniform_int_distribution<int> bl (1, maxBlock);
    for (size_t pos = 0; pos < n + (size_t) L;)
    {
        const int b = (int) std::min<size_t> (rng ? (size_t) bl (*rng) : 512u, n + (size_t) L - pos);
        float* p[2] {};
        for (size_t c = 0; c < x.size(); ++c)
            p[c] = x[c].data() + pos;
        countAllocs = count;
        d.process (p, (int) x.size(), b);
        countAllocs = false;
        pos += (size_t) b;
    }
    for (auto& c : x)
        c.erase (c.begin(), c.begin() + L);
    if (curve != nullptr)
        for (int b = 0; b < 23; ++b)
            (*curve)[(size_t) b] = d.trueToneDb (b);
    return x;
}

int main (int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp (argv[1], "--quick") == 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto wb = readFile ("Resources/dereverb.bin");
    Weights W;
    check ("weights: Resources/dereverb.bin loads", W.load (wb.data(), wb.size()) ? 1 : 0, 1, 1, "");
    {
        Weights bad;
        auto t = wb;
        t[0] ^= 0x55;
        const bool r1 = bad.load (t.data(), t.size());
        const bool r2 = bad.load (wb.data(), wb.size() - 4);
        auto nanw = wb;
        const float nanv = std::numeric_limits<float>::quiet_NaN();
        std::memcpy (nanw.data() + 64, &nanv, 4);
        const bool r3 = bad.load (nanw.data(), nanw.size());
        check ("weights: a damaged file is refused (bad magic, truncated, NaN)", (! r1 && ! r2 && ! r3) ? 1 : 0, 1, 1, "");
    }
    if (! W.ok)
        return 1;

    // 1. parity with the trained PyTorch model (reference written by research/export_dr.py --ref)
    {
        auto in = readF32 ("tests/reference/parity_in.f32");
        auto ref = readF32 ("tests/reference/parity_out.f32");
        auto out = render (W, 48000, { in }, 1.0f, false)[0];
        double e = 0, r = 0, mx = 0;
        for (size_t i = 4096; i + 4096 < std::min (ref.size(), out.size()); ++i)
        {
            e += (double) (out[i] - ref[i]) * (out[i] - ref[i]);
            r += (double) ref[i] * ref[i];
            mx = std::max (mx, (double) std::abs (out[i] - ref[i]));
        }
        check ("parity with the trained PyTorch model: error re the reference output", db (e, r), -300, -100, "dB");
    }

    const int rates[] = { 44100, 48000, 88200, 96000 };
    std::mt19937 rng (5);
    std::normal_distribution<float> nd;
    for (int fs : rates)
    {
        if (quick && fs != 48000 && fs != 44100)
            continue;
        char R[32];
        std::snprintf (R, sizeof R, " @ %.1f kHz", fs / 1000.0);
        const size_t n = (size_t) fs * 3;
        std::vector<float> tone (n);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / fs;
            tone[i] = (float) (0.2 * std::sin (2 * M_PI * 440 * t) + 0.1 * std::sin (2 * M_PI * 3100 * t) + 0.05 * std::sin (2 * M_PI * 9000 * t)
                               + 0.03 * std::sin (2 * M_PI * 15000 * t));
        }
        // 2. transparency at REDUCTION 0, random block sizes (constant latency)
        {
            auto out = render (W, fs, { tone, tone }, 0.0f, false, &rng);
            double e = 0, r = 0;
            for (size_t i = (size_t) fs / 2; i < n - (size_t) fs / 4; ++i)  // away from the start and the abrupt end
            {
                e += (double) (out[0][i] - tone[i]) * (out[0][i] - tone[i]);
                r += (double) tone[i] * tone[i];
            }
            check (std::string ("REDUCTION 0: output = input delayed by the reported latency (random blocks 1-2048)") + R, db (e, r), -400, -120, "dB");
        }
        // 3. silence in -> silence out (exactly), and NaN / Inf in -> finite out
        {
            std::vector<float> z (n, 0.0f);
            auto out = render (W, fs, { z, z }, 1.0f, false);
            double mx = 0;
            for (auto& c : out)
                for (float v : c)
                    mx = std::max (mx, (double) std::abs (v));
            check (std::string ("silence in -> silence out (max |y|)") + R, mx, 0, 0, "");
            auto bad = tone;
            for (size_t i = 1000; i < n; i += 997)
                bad[i] = (i / 997) % 2 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
            auto o2 = render (W, fs, { bad, bad }, 1.0f, false);
            int nonfinite = 0;
            for (auto& c : o2)
                for (float v : c)
                    nonfinite += std::isfinite (v) ? 0 : 1;
            check (std::string ("NaN / Inf in: non-finite output samples") + R, nonfinite, 0, 0, "");
        }
        // 4. LISTEN: output + removed = the input delayed (REDUCTION 100 %)
        {
            std::vector<float> x (n);
            for (size_t i = 0; i < n; ++i)
                x[i] = tone[i] * (0.5f + 0.5f * (float) std::sin (2 * M_PI * 0.7 * (double) i / fs)) + 0.01f * nd (rng);
            // the default REDUCTION, both controls reached through the smoothing (a float one-pole that never
            // arrived left LISTEN at 0.99996: -88 dB, found by the independent audit)
            auto o = render (W, fs, { x }, 0.85f, false, nullptr, 2048, false, true)[0];
            auto l = render (W, fs, { x }, 0.85f, true, nullptr, 2048, false, true)[0];
            double e = 0, r = 0;
            for (size_t i = (size_t) fs / 2; i < n - (size_t) fs / 4; ++i)
            {
                const double s = (double) o[i] + l[i] - x[i];
                e += s * s;
                r += (double) x[i] * x[i];
            }
            check (std::string ("LISTEN: processed + removed = the input, full band (error re the input)") + R, db (e, r), -400, -110, "dB");
        }
        // 4b. digital silence before a take (a DAW project's start) changes nothing: the take comes out exactly as
        //     without it (silence of a whole number of hops keeps the frame grid)
        if (fs == 48000)
        {
            std::vector<float> x (n);
            for (size_t i = 0; i < n; ++i)
                x[i] = tone[i] * (0.5f + 0.5f * (float) std::sin (2 * M_PI * 0.7 * (double) i / fs)) + 0.01f * nd (rng);
            const size_t lead = 40 * 512;
            std::vector<float> xl (lead, 0.0f);
            xl.insert (xl.end(), x.begin(), x.end());
            auto a = render (W, fs, { x }, 1.0f, false)[0];
            auto b = render (W, fs, { xl }, 1.0f, false)[0];
            double e = 0, r = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double d = (double) b[i + lead] - a[i];
                e += d * d;
                r += (double) a[i] * a[i];
            }
            check (std::string ("0.43 s of digital silence first: the take comes out identical") + R, db (e, r), -400, -140, "dB");
        }
        // 5. stereo channels are independent: signal on the left only, the right stays silent
        {
            std::vector<float> z (n, 0.0f);
            auto out = render (W, fs, { tone, z }, 1.0f, false);
            double mx = 0;
            for (float v : out[1])
                mx = std::max (mx, (double) std::abs (v));
            check (std::string ("stereo: left only in -> right channel silent") + R, mx, 0, 0, "");
        }
    }
    // 6. real-time safety: no allocation in process (any rate, random blocks)
    {
        std::vector<float> x ((size_t) 44100 * 2);
        for (auto& v : x)
            v = 0.1f * nd (rng);
        allocCount = 0;
        render (W, 44100, { x, x }, 0.7f, false, &rng, 2048, true);
        render (W, 48000, { x, x }, 0.7f, true, &rng, 2048, true);
        check ("no memory allocation in process() (44.1 / 48 kHz, random blocks)", (double) allocCount, 0, 0, "allocs");
    }
    // 8. 3.0 LIVE: the causal network as a zero-latency minimum-phase filter
    {
        auto lb = readFile ("Resources/dereverb_live.bin");
        Weights WL;
        check ("LIVE weights: Resources/dereverb_live.bin loads, a causal network (no look-ahead)",
               (WL.load (lb.data(), lb.size()) && WL.la == 0 && W.la == kLa) ? 1 : 0, 1, 1, "");
        if (WL.ok)
        {
            for (int fs : { 44100, 48000, 96000 })
            {
                char R[32];
                std::snprintf (R, sizeof R, " @ %.1f kHz", fs / 1000.0);
                DeReverb d;
                d.prepare (WL, fs, 2, 2048);
                check (std::string ("LIVE: latency (ms; 0 at 48 kHz, the resamplers' elsewhere)") + R, 1000.0 * d.latencySamples() / fs,
                       0.0, fs == 48000 ? 0.0 : 4.0, "ms");
                const size_t n = (size_t) fs * 3;
                std::vector<float> tone (n);
                for (size_t i = 0; i < n; ++i)
                    tone[i] = (float) (0.2 * std::sin (2 * M_PI * 440.0 * i / fs) + 0.05 * std::sin (2 * M_PI * 9000.0 * i / fs));
                auto out = render (WL, fs, { tone, tone }, 0.0f, false, &rng);
                double e = 0, r = 0;
                for (size_t i = (size_t) fs / 2; i < n - (size_t) fs / 4; ++i)
                {
                    e += (double) (out[0][i] - tone[i]) * (out[0][i] - tone[i]);
                    r += (double) tone[i] * tone[i];
                }
                check (std::string ("LIVE, REDUCTION 0: output = input (random blocks)") + R, db (e, r), -400, -120, "dB");
                std::vector<float> noisy (n);
                for (size_t i = 0; i < n; ++i)
                    noisy[i] = tone[i] + 0.05f * nd (rng);
                auto pr = render (WL, fs, { noisy, noisy }, 1.0f, false, &rng);
                auto rm = render (WL, fs, { noisy, noisy }, 1.0f, true, &rng);
                double e2 = 0, r2 = 0, mx = 0;
                bool finite = true;
                for (size_t i = (size_t) fs / 2; i < n - (size_t) fs / 4; ++i)
                {
                    const double d2 = (double) pr[0][i] + rm[0][i] - noisy[i];
                    e2 += d2 * d2;
                    r2 += (double) noisy[i] * noisy[i];
                    mx = std::max (mx, (double) std::abs (pr[0][i]));
                    finite = finite && std::isfinite (pr[0][i]) && std::isfinite (rm[0][i]);
                }
                check (std::string ("LIVE: processed + removed (LISTEN) = the input (error re the input)") + R, db (e2, r2), -400, -100, "dB");
                check (std::string ("LIVE: output finite and bounded (max |y|)") + R, finite ? mx : 1e9, 0.0, 1.0, "");
                std::vector<float> z (n, 0.0f);
                auto zo = render (WL, fs, { z, z }, 1.0f, false, &rng);
                double zm = 0;
                for (float v : zo[0])
                    zm = std::max (zm, (double) std::abs (v));
                check (std::string ("LIVE: silence in -> silence out (max |y|)") + R, zm, 0, 0, "");
            }
            std::vector<float> x ((size_t) 48000 * 2);
            for (auto& v : x)
                v = 0.1f * nd (rng);
            allocCount = 0;
            render (WL, 48000, { x, x }, 0.7f, false, &rng, 2048, true);
            render (WL, 44100, { x, x }, 0.7f, true, &rng, 2048, true);
            check ("LIVE: no memory allocation in process() (44.1 / 48 kHz, random blocks)", (double) allocCount, 0, 0, "allocs");
            if (! quick)
            {
                const auto a = std::chrono::steady_clock::now();
                std::vector<float> y ((size_t) 48000 * 10);
                for (auto& v : y)
                    v = 0.1f * nd (rng);
                render (WL, 48000, { y, y }, 1.0f, false);
                const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - a).count();
                check ("LIVE: CPU, stereo @ 48000 Hz (% of one core)", 100.0 * sec / 10.0, 0, 30, "%");
            }
        }
    }
    // 8. 4.0 TRUE TONE (Studio and LIVE): safe, bounded, block-size invariant, the mix identities still hold
    {
        Weights WL2;
        auto wl = readFile ("Resources/dereverb_live.bin");
        WL2.load (wl.data(), wl.size());
        // a voice in a small room: 110-220 Hz glides with 20 harmonics, syllables, convolved with a 0.35 s decay
        const int fs = 48000;
        const size_t n = (size_t) fs * 8;
        std::vector<float> v (n, 0.0f), x (n, 0.0f);
        double ph = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / fs, f0 = 110.0 + 55.0 * (1.0 + std::sin (2 * M_PI * 0.3 * t));
            ph += 2 * M_PI * f0 / fs;
            double s = 0.0;
            for (int k = 1; k <= 20; ++k)
                s += std::sin (k * ph) / k;
            const double env = std::max (0.0, std::sin (2 * M_PI * 2.5 * t));
            v[i] = (float) (0.1 * s * env);
        }
        std::vector<float> h ((size_t) (0.35 * fs));
        for (size_t i = 0; i < h.size(); ++i)
            h[i] = (i == 0 ? 1.0f : 0.25f * nd (rng)) * (float) std::exp (-6.9 * (double) i / (0.35 * fs));
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < h.size() && j <= i; j += 7)   // a sparse room (every 7th tap): enough reverb, quick
                x[i] += v[i - j] * h[j] * (j == 0 ? 1.0f : 2.6f);
        for (int live = 0; live < 2; ++live)
        {
            const Weights& WW = live ? WL2 : W;
            const std::string M = live ? " (LIVE)" : " (Studio)";
            std::array<float, 23> curve {};
            auto a = render (WW, fs, { x, x }, 1.0f, false, nullptr, 2048, false, false, true, &curve);
            auto b = render (WW, fs, { x, x }, 1.0f, false, &rng, 2048, false, false, true);
            double d = 0.0, mx = 0.0, cmax = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                d = std::max (d, (double) std::abs (a[0][i] - b[0][i]));
                mx = std::max (mx, (double) std::abs (a[0][i]));
            }
            for (float c : curve)
                cmax = std::max (cmax, (double) std::abs (c));
            check ("TRUE TONE: blocks of 512 vs random 1-2048 (max difference)" + M, d, 0.0, 1.0e-6, "");
            check ("TRUE TONE: the correction stays within +-3 dB (largest band, after 8 s)" + M, cmax, 0.0, 3.0001, "dB");
            double xm = 0.0;
            for (float s0 : x)
                xm = std::max (xm, (double) std::abs (s0));
            check ("TRUE TONE: output peak re the input's on a voice in a room" + M, mx / xm, 0.0, 1.5, "x");
            std::vector<float> z (n / 4, 0.0f);
            auto zo = render (WW, fs, { z, z }, 1.0f, false, nullptr, 2048, false, false, true);
            double zm = 0.0;
            for (float s : zo[0])
                zm = std::max (zm, (double) std::abs (s));
            check ("TRUE TONE: silence in -> silence out (max |y|)" + M, zm, 0.0, 0.0, "");
            auto r0 = render (WW, fs, { x }, 0.0f, false, nullptr, 2048, false, false, true)[0];
            double e = 0.0, r = 0.0;
            for (size_t i = (size_t) fs / 2; i < n - (size_t) fs / 4; ++i)
            {
                e += (double) (r0[i] - x[i]) * (r0[i] - x[i]);
                r += (double) x[i] * x[i];
            }
            check ("TRUE TONE: REDUCTION 0 is still the input (error re the input)" + M, db (e, r), -400, -120, "dB");
            auto o = render (WW, fs, { x }, 0.85f, false, nullptr, 2048, false, true, true)[0];
            auto l = render (WW, fs, { x }, 0.85f, true, nullptr, 2048, false, true, true)[0];
            e = r = 0.0;
            for (size_t i = (size_t) fs / 2; i < n - (size_t) fs / 4; ++i)
            {
                const double s2 = (double) o[i] + l[i] - x[i];
                e += s2 * s2;
                r += (double) x[i] * x[i];
            }
            check ("TRUE TONE: LISTEN - processed + removed = the input (error re the input)" + M, db (e, r), -400, -110, "dB");
            allocCount = 0;
            render (WW, 44100, { x, x }, 0.7f, false, &rng, 2048, true, false, true);
            check ("TRUE TONE: no memory allocation in process() (44.1 kHz, random blocks)" + M, (double) allocCount, 0, 0, "allocs");
        }
    }
    // 7. CPU: 48 kHz stereo and 44.1 kHz stereo, % of one core
    if (! quick)
        for (int fs : { 48000, 44100 })
        {
            std::vector<float> x ((size_t) fs * 10);
            for (auto& v : x)
                v = 0.1f * nd (rng);
            const auto a = std::chrono::steady_clock::now();
            render (W, fs, { x, x }, 1.0f, false);
            const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - a).count();
            check ("CPU, stereo @ " + std::to_string (fs) + " Hz (% of one core)", 100.0 * sec / 10.0, 0, 25, "%");
        }
    const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    if (fails == 0)
        std::printf ("ALL PASS: %d of %d checks passed (%.1f s)\n", checks, checks, sec);
    else
        std::printf ("FAILED: %d of %d checks (%.1f s)\n", fails, checks, sec);
    return fails;
}
