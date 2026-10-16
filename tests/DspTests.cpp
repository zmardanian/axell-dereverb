// Axell De-Reverb - DSP test suite (JUCE-free): the engine headers exactly as they ship.
// usage: DrDspTests [--quick]   (run from the repository root: reads Resources/dereverb.bin and tests/reference)
#include "DeReverb.h"

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
                                               bool ramp = false)
{
    DeReverb d;
    d.prepare (W, fs, (int) x.size(), maxBlock);
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
