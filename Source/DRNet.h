#pragma once
// Axell De-Reverb - the network, streaming inference (framework independent, no JUCE).
//
// Mirrors research/dr_model.py exactly (a test compares the two to float precision):
//   STFT 1024 / 512 at 48 kHz (sqrt-Hann), 513 bins; per frame the features of research/dr_model.features_np:
//   32 ERB-band log powers minus their running mean (/4) and bins 0..95 over their running mean magnitude;
//   input = features of frames t-LA-CTX .. t (newest last) -> Linear+ReLU -> GRU -> GRU (+ residual) ->
//   32 ERB gains (sigmoid), deep-filter coefficients for bins 0..95 (tanh, order 5 over frames t-LA-4 .. t-LA) and a
//   per-bin mix (sigmoid). Output frame = input frame t - LA (LA frames of look-ahead).
// LA comes with the weights: 3 (STUDIO, 2.0's network) or 0 (3.0 LIVE: a causal network, fine-tuned from 2.0's).
// Weights: a flat float32 file written by research/export_dr.py (layout below). Real-time safe after load().
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace axdr
{
constexpr int kFs = 48000, kNfft = 1024, kHop = 512, kNb = kNfft / 2 + 1;
constexpr int kNerb = 32, kNdf = 96, kDfOrder = 5, kLa = 3, kCtx = 2, kH = 256;   // kLa: the largest look-ahead
constexpr int kFeat = kNerb + 2 * kNdf;
constexpr int kIn = (kCtx + 1 + kLa) * kFeat;                                      // the largest input

using cf = std::complex<float>;

// dot product with 8 independent accumulators: vectorises without fast-math (the summation order is fixed, so the
// result is deterministic; it differs from a serial sum by rounding only)
#if defined(__GNUC__) && defined(__x86_64__) && ! defined(__clang__) && ! defined(__SANITIZE_ADDRESS__)
__attribute__ ((target_clones ("arch=x86-64-v3", "default")))
#endif
inline float dot (const float* a, const float* b, int n) noexcept
{
    float s[8] {};
    int i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; ++k)
            s[k] += a[i + k] * b[i + k];
    float t = ((s[0] + s[4]) + (s[1] + s[5])) + ((s[2] + s[6]) + (s[3] + s[7]));
    for (; i < n; ++i)
        t += a[i] * b[i];
    return t;
}

struct DenseLayer
{
    int in = 0, out = 0;
    std::vector<float> w, b; // w: out x in (row-major, like torch)
    void apply (const float* x, float* y) const noexcept
    {
        for (int o = 0; o < out; ++o)
            y[o] = b[(size_t) o] + dot (w.data() + (size_t) o * (size_t) in, x, in);
    }
};

struct GruLayer
{
    int in = 0, hid = 0;
    std::vector<float> wih, whh, bih, bhh; // torch layout: (3H x in), (3H x H), gate order r, z, n
    void step (const float* x, float* h, float* tmp) const noexcept
    {
        // tmp: 6H scratch
        float* gi = tmp;
        float* gh = tmp + 3 * hid;
        for (int o = 0; o < 3 * hid; ++o)
        {
            gi[o] = bih[(size_t) o] + dot (wih.data() + (size_t) o * (size_t) in, x, in);
            gh[o] = bhh[(size_t) o] + dot (whh.data() + (size_t) o * (size_t) hid, h, hid);
        }
        for (int j = 0; j < hid; ++j)
        {
            const float r = 1.0f / (1.0f + std::exp (-(gi[j] + gh[j])));
            const float z = 1.0f / (1.0f + std::exp (-(gi[hid + j] + gh[hid + j])));
            const float n = std::tanh (gi[2 * hid + j] + r * gh[2 * hid + j]);
            h[j] = (1.0f - z) * n + z * h[j];
        }
    }
};

struct Weights
{
    DenseLayer inp, outG, outDf, outA;
    GruLayer gru1, gru2;
    int la = kLa;            // look-ahead frames of this network (from the input layer's width)
    bool ok = false;

    // layout (float32, little endian): magic 'AXDR', version 1, then each tensor as [rows, cols] (int32) + data:
    // inp.w, inp.b, gru1.wih, gru1.whh, gru1.bih, gru1.bhh, gru2.(same), outG.w, outG.b, outDf.w, outDf.b, outA.w, outA.b
    bool load (const void* data, size_t bytes)
    {
        ok = false;
        const auto* p = static_cast<const uint8_t*> (data);
        size_t pos = 0;
        auto rdI = [&] (int32_t& v) { if (pos + 4 > bytes) return false; std::memcpy (&v, p + pos, 4); pos += 4; return true; };
        auto rdT = [&] (std::vector<float>& v, int rows, int cols) {
            int32_t r = 0, c = 0;
            if (! rdI (r) || ! rdI (c) || r != rows || c != cols)
                return false;
            const size_t n = (size_t) r * (size_t) c;
            if (pos + n * 4 > bytes)
                return false;
            v.resize (n);
            std::memcpy (v.data(), p + pos, n * 4);
            pos += n * 4;
            for (float f : v)
                if (! std::isfinite (f))
                    return false;
            return true;
        };
        int32_t magic = 0, ver = 0;
        if (! rdI (magic) || magic != 0x52445841 || ! rdI (ver) || ver != 1)
            return false;
        // the input layer's width tells the look-ahead: (CTX + 1 + LA) frames of features
        {
            int32_t r = 0, c = 0;
            if (pos + 8 > bytes)
                return false;
            std::memcpy (&r, p + pos, 4);
            std::memcpy (&c, p + pos + 4, 4);
            if (r != kH || c % kFeat != 0 || c / kFeat - kCtx - 1 < 0 || c / kFeat - kCtx - 1 > kLa)
                return false;
            la = c / kFeat - kCtx - 1;
        }
        const int nIn = (kCtx + 1 + la) * kFeat;
        inp.in = nIn; inp.out = kH;
        gru1.in = kH; gru1.hid = kH; gru2.in = kH; gru2.hid = kH;
        outG.in = kH; outG.out = kNerb; outDf.in = kH; outDf.out = kNdf * kDfOrder * 2; outA.in = kH; outA.out = kNdf;
        ok = rdT (inp.w, kH, nIn) && rdT (inp.b, 1, kH)
             && rdT (gru1.wih, 3 * kH, kH) && rdT (gru1.whh, 3 * kH, kH) && rdT (gru1.bih, 1, 3 * kH) && rdT (gru1.bhh, 1, 3 * kH)
             && rdT (gru2.wih, 3 * kH, kH) && rdT (gru2.whh, 3 * kH, kH) && rdT (gru2.bih, 1, 3 * kH) && rdT (gru2.bhh, 1, 3 * kH)
             && rdT (outG.w, kNerb, kH) && rdT (outG.b, 1, kNerb)
             && rdT (outDf.w, kNdf * kDfOrder * 2, kH) && rdT (outDf.b, 1, kNdf * kDfOrder * 2)
             && rdT (outA.w, kNdf, kH) && rdT (outA.b, 1, kNdf)
             && pos == bytes;
        return ok;
    }
};

// ERB band of every bin (same construction as dr_model.erb_matrix)
inline std::array<int, kNb> erbBands()
{
    std::array<int, kNb> idx {};
    auto erb = [] (double f) { return 21.4 * std::log10 (1.0 + 0.00437 * f); };
    const double top = erb (kFs / 2.0);
    std::array<double, kNerb + 1> edges {};
    for (int b = 0; b <= kNerb; ++b)
        edges[(size_t) b] = top * b / kNerb;
    std::array<double, kNb> e {};
    for (int k = 0; k < kNb; ++k)
    {
        e[(size_t) k] = erb (kFs / 2.0 * k / (kNb - 1));
        // searchsorted(edges, e, side="right") - 1, clipped
        int i = (int) (std::upper_bound (edges.begin(), edges.end(), e[(size_t) k]) - edges.begin()) - 1;
        idx[(size_t) k] = std::clamp (i, 0, kNerb - 1);
    }
    // empty bands take the bin nearest their lower edge (as in python)
    for (int b = 0; b < kNerb; ++b)
    {
        bool any = false;
        for (int k = 0; k < kNb; ++k)
            any = any || idx[(size_t) k] == b;
        if (! any)
        {
            int best = 0;
            for (int k = 1; k < kNb; ++k)
                if (std::abs (edges[(size_t) b] - e[(size_t) k]) < std::abs (edges[(size_t) b] - e[(size_t) best]))
                    best = k;
            idx[(size_t) best] = b;
        }
    }
    return idx;
}

// one network instance: per-frame features, context, recurrent state, deep-filter history
class Net
{
public:
    void prepare (const Weights& w)
    {
        W = &w;
        la = w.la;
        band = erbBands();
        bandCount.fill (0);
        for (int k = 0; k < kNb; ++k)
            ++bandCount[(size_t) band[(size_t) k]];
        ctx.assign ((size_t) (kCtx + 1 + la) * kFeat, 0.0f);
        xin.assign ((size_t) kIn, 0.0f);
        z0.assign (kH, 0.0f); z1.assign (kH, 0.0f); h1.assign (kH, 0.0f); h2.assign (kH, 0.0f); h2out.assign (kH, 0.0f);
        tmp.assign (6 * kH, 0.0f);
        g.assign (kNerb, 0.0f); df.assign ((size_t) kNdf * kDfOrder * 2, 0.0f); amix.assign (kNdf, 0.0f);
        specHist.assign ((size_t) (la + kDfOrder) * kNb, cf {});
        reset();
    }

    void reset() noexcept
    {
        std::fill (ctx.begin(), ctx.end(), 0.0f);
        std::fill (h1.begin(), h1.end(), 0.0f);
        std::fill (h2.begin(), h2.end(), 0.0f);
        std::fill (specHist.begin(), specHist.end(), cf {});
        histPos = 0;
        first = true;
    }

    // one STFT frame in (input frame t), one frame out (the enhanced input frame t - LA)
    void frame (const cf* X, cf* Y) noexcept
    {
        // digital silence (a DAW before the first take, a gate in front; training never had any): until the first
        // frame that is not silent the network stays as at the start (the stream effectively starts there), and
        // later silent frames do not move the running means. research/dr_model.py does the same.
        double pm = 0.0;
        for (int k = 0; k < kNb; ++k)
            pm += (double) std::norm (X[k]);
        const bool silent = pm / kNb < kSilentPower;
        if (first && silent)
        {
            reset();
            std::fill (Y, Y + kNb, cf {});
            return;
        }
        const bool track = first || ! silent;
        // keep the spectrum history (input frames t .. t - LA - DF_ORDER + 1)
        const int HN = la + kDfOrder;
        std::copy (X, X + kNb, specHist.begin() + (size_t) histPos * kNb);
        auto hist = [&] (int back) { return specHist.data() + (size_t) ((histPos - back + HN) % HN) * kNb; };
        // features of frame t
        float feat[kFeat];
        std::array<double, kNerb> ep {};
        for (int k = 0; k < kNb; ++k)
            ep[(size_t) band[(size_t) k]] += (double) std::norm (X[k]);
        for (int b = 0; b < kNerb; ++b)
        {
            const double e = std::log10 (ep[(size_t) b] / std::max (1, bandCount[(size_t) b]) + 1.0e-10);
            if (track)
                m[(size_t) b] = first ? e : kAlpha * m[(size_t) b] + (1.0 - kAlpha) * e;
            feat[b] = (float) ((e - m[(size_t) b]) / 4.0);
        }
        for (int k = 0; k < kNdf; ++k)
        {
            const double mag = std::sqrt ((double) std::norm (X[k]) + 1.0e-10);
            if (track)
                u[(size_t) k] = first ? mag : kAlpha * u[(size_t) k] + (1.0 - kAlpha) * mag;
            const double d = std::max (u[(size_t) k], 1.0e-5);
            feat[kNerb + k] = (float) (X[k].real() / d);
            feat[kNerb + kNdf + k] = (float) (X[k].imag() / d);
        }
        first = false;
        // context window: shift left, newest last
        std::memmove (ctx.data(), ctx.data() + kFeat, (size_t) (kCtx + la) * kFeat * sizeof (float));
        std::copy (feat, feat + kFeat, ctx.data() + (size_t) (kCtx + la) * kFeat);
        // network
        W->inp.apply (ctx.data(), z0.data());
        for (auto& v : z0)
            v = std::max (0.0f, v);
        W->gru1.step (z0.data(), h1.data(), tmp.data());
        W->gru2.step (h1.data(), h2.data(), tmp.data());
        for (int j = 0; j < kH; ++j)
            z1[(size_t) j] = h1[(size_t) j] + h2[(size_t) j];
        W->outG.apply (z1.data(), g.data());
        W->outDf.apply (z1.data(), df.data());
        W->outA.apply (z1.data(), amix.data());
#ifdef AXDR_DEBUG
        if (dbgFrame-- == 0)
        {
            std::printf ("FEAT"); for (int i = 0; i < kFeat; ++i) std::printf (" %.6g", feat[i]); std::printf ("\n");
            std::printf ("Z0"); for (int i = 0; i < kH; ++i) std::printf (" %.6g", z0[(size_t) i]); std::printf ("\n");
            std::printf ("G"); for (int i = 0; i < kNerb; ++i) std::printf (" %.6g", g[(size_t) i]); std::printf ("\n");
        }
#endif
        // the frame being produced: input frame t - LA
        const cf* Xd = hist (la);
        for (int k = 0; k < kNb; ++k)
        {
            const float gk = 1.0f / (1.0f + std::exp (-g[(size_t) band[(size_t) k]]));
            Y[k] = Xd[k] * gk;
        }
        for (int k = 0; k < kNdf; ++k)
        {
            cf s {};
            for (int i = 0; i < kDfOrder; ++i)
            {
                // coefficient i multiplies frame (t - LA) - (ORDER - 1) + i  (oldest first)
                const cf* Xi = hist (la + kDfOrder - 1 - i);
                const float re = std::tanh (df[(size_t) ((k * kDfOrder + i) * 2)]);
                const float im = std::tanh (df[(size_t) ((k * kDfOrder + i) * 2 + 1)]);
                s += Xi[k] * cf (re, im);
            }
            const float a = 1.0f / (1.0f + std::exp (-amix[(size_t) k]));
            Y[k] = a * s + (1.0f - a) * Y[k];
        }
        histPos = (histPos + 1) % HN;
    }

#ifdef AXDR_DEBUG
    int dbgFrame = 40;
#endif
private:
    static constexpr double kAlpha = 0.99;
    static constexpr double kSilentPower = 512.0 * 1.0e-13; // mean bin power of white noise at -130 dBFS (sqrt-Hann, 1024)
    const Weights* W = nullptr;
    int la = kLa;
    std::array<int, kNb> band {};
    std::array<int, kNerb> bandCount {};
    std::array<double, kNerb> m {};
    std::array<double, kNdf> u {};
    std::vector<float> ctx, xin, z0, z1, h1, h2, h2out, tmp, g, df, amix;
    std::vector<cf> specHist;
    int histPos = 0;
    bool first = true;
};
} // namespace axdr
