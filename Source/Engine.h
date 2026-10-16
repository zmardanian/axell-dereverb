#pragma once
// Axell De-Reverb - the engine (framework independent, no JUCE): streaming STFT (1024 / 512, sqrt-Hann analysis and
// synthesis, 50 % overlap-add), the network per frame, inverse STFT. Mono detection path; each channel gets its own
// network instance (a stereo vocal stays stereo). Latency: one frame (1024) + LA hops of look-ahead.
// REDUCTION (0..1) mixes the processed signal with the latency-aligned input here, at 48 kHz, so dry and processed
// paths stay sample-aligned whatever the host rate; LISTEN plays what is removed (input - processed) x REDUCTION.
// Real-time safe after prepare(): no allocation, locks or I/O in process().
#include <axell/dsp/Fft.h>

#include <atomic>

#include "DRNet.h"

namespace axdr
{
class Engine
{
public:
    static constexpr int kMaxCh = 2;

    bool prepare (const Weights& w, int numChannels)
    {
        if (! w.ok)
            return false;
        nch = std::clamp (numChannels, 1, kMaxCh);
        fft.prepare (kNfft);
        for (int i = 0; i < kNfft; ++i)
            win[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / kNfft));
        for (int c = 0; c < nch; ++c)
        {
            net[(size_t) c].prepare (w);
            inBuf[(size_t) c].assign (kNfft, 0.0f);
            outAcc[(size_t) c].assign (kNfft, 0.0f);
            outQ[(size_t) c].assign (kHop, 0.0f);
            dry[(size_t) c].assign ((size_t) latency(), 0.0f);
        }
        frameIn.assign (kNfft, 0.0f);
        spec.assign (kNfft + 2, 0.0f);
        X.assign (kNb, cf {});
        Y.assign (kNb, cf {});
        timeOut.assign (kNfft, 0.0f);
        reset();
        return true;
    }

    void reset() noexcept
    {
        for (int c = 0; c < nch; ++c)
        {
            net[(size_t) c].reset();
            std::fill (inBuf[(size_t) c].begin(), inBuf[(size_t) c].end(), 0.0f);
            std::fill (outAcc[(size_t) c].begin(), outAcc[(size_t) c].end(), 0.0f);
            std::fill (outQ[(size_t) c].begin(), outQ[(size_t) c].end(), 0.0f);
            std::fill (dry[(size_t) c].begin(), dry[(size_t) c].end(), 0.0f);
        }
        fill = 0;
        dryPos = 0;
        accIn = accRm = accOut = smIn = smRm = smOut = 0.0;
        amt = amtTarget;
        lis = lisTarget;
    }

    // input sample n comes out at sample n + latency()
    static constexpr int latency() noexcept { return kNfft + kLa * kHop; }

    void setAmount (float a) noexcept { amtTarget = std::clamp (a, 0.0f, 1.0f); }
    // meters (any thread): input / output level (dBFS, ~0.5 s) and the removed room re the input (dB)
    float inputDb() const noexcept { return meterIn.load (std::memory_order_relaxed); }
    float outputDb() const noexcept { return meterOut.load (std::memory_order_relaxed); }
    float removedDb() const noexcept { return meterRemoved.load (std::memory_order_relaxed); }
    void setListen (bool on) noexcept { lisTarget = on ? 1.0f : 0.0f; }

    void process (float* const* io, int numChannels, int n) noexcept
    {
        const int ch = std::min (numChannels, nch);
        for (int i = 0; i < n; ++i)
        {
            amt += (amtTarget - amt) * kSmooth;
            lis += (lisTarget - lis) * kSmooth;
            // a float one-pole stalls ~6e-5 short of its target (the step falls below half an ulp), so snap from
            // 1e-4: an -80 dB step, inaudible, and the mix then is exact (processed + removed = the input)
            if (std::abs (amt - amtTarget) < 1.0e-4f) amt = amtTarget;
            if (std::abs (lis - lisTarget) < 1.0e-4f) lis = lisTarget;
            for (int c = 0; c < ch; ++c)
            {
                float& s = io[c][i];
                const float x = std::isfinite (s) ? std::clamp (s, -1000.0f, 1000.0f) : 0.0f;
                inBuf[(size_t) c][(size_t) (kNfft - kHop + fill)] = x;
                auto& d = dry[(size_t) c];
                const float dr = d[(size_t) dryPos];       // the input, latency() samples ago
                d[(size_t) dryPos] = x;
                const float wet = outQ[(size_t) c][(size_t) fill];
                const float removed = dr - wet;
                const float mixed = dr - amt * removed;     // REDUCTION
                s = mixed + lis * (amt * removed - mixed);  // LISTEN: what is removed
                accIn += (double) dr * dr;
                accRm += (double) (amt * removed) * (amt * removed);
                accOut += (double) s * s;
            }
            dryPos = (dryPos + 1) % latency();
            if (++fill == kHop)
            {
                fill = 0;
                for (int c = 0; c < ch; ++c)
                    hop (c);
                // meters, smoothed over ~0.5 s (hops of 10.7 ms)
                const double k = 0.98;
                smIn = k * smIn + (1 - k) * accIn;
                smRm = k * smRm + (1 - k) * accRm;
                smOut = k * smOut + (1 - k) * accOut;
                auto db = [] (double e) { return e > 1.0e-14 ? (float) (10.0 * std::log10 (e / kHop)) : -120.0f; };
                meterIn.store (db (smIn / ch), std::memory_order_relaxed);
                meterOut.store (db (smOut / ch), std::memory_order_relaxed);
                meterRemoved.store (smIn > 1.0e-14 ? (float) (10.0 * std::log10 ((smRm + 1.0e-20) / smIn)) : -120.0f, std::memory_order_relaxed);
                accIn = accRm = accOut = 0.0;
            }
        }
    }

private:
    void hop (int c) noexcept
    {
        auto& ib = inBuf[(size_t) c];
        for (int i = 0; i < kNfft; ++i)
            frameIn[(size_t) i] = ib[(size_t) i] * win[(size_t) i];
        fft.forward (frameIn.data(), spec.data());
        for (int k = 0; k < kNb; ++k)
            X[(size_t) k] = cf (spec[(size_t) (2 * k)], spec[(size_t) (2 * k + 1)]);
        net[(size_t) c].frame (X.data(), Y.data());
        // a real signal's DC and Nyquist bins are real (the deep filter's complex taps can rotate them)
        Y[0] = cf (Y[0].real(), 0.0f);
        Y[(size_t) kNb - 1] = cf (Y[(size_t) kNb - 1].real(), 0.0f);
        for (int k = 0; k < kNb; ++k)
        {
            spec[(size_t) (2 * k)] = Y[(size_t) k].real();
            spec[(size_t) (2 * k + 1)] = Y[(size_t) k].imag();
        }
        fft.inverse (spec.data(), timeOut.data());
        auto& acc = outAcc[(size_t) c];
        for (int i = 0; i < kNfft; ++i)
            acc[(size_t) i] += timeOut[(size_t) i] * win[(size_t) i];
        // the first hop of the accumulator is complete: it plays during the next hop
        std::copy (acc.begin(), acc.begin() + kHop, outQ[(size_t) c].begin());
        std::copy (acc.begin() + kHop, acc.end(), acc.begin());
        std::fill (acc.begin() + kNfft - kHop, acc.end(), 0.0f);
        // slide the input
        std::copy (ib.begin() + kHop, ib.end(), ib.begin());
    }

    static constexpr float kSmooth = 0.0005f; // ~40 ms at 48 kHz
    int nch = 1, fill = 0, dryPos = 0;
    float amt = 1.0f, amtTarget = 1.0f, lis = 0.0f, lisTarget = 0.0f;
    std::array<std::vector<float>, kMaxCh> dry;
    double accIn = 0, accRm = 0, accOut = 0, smIn = 0, smRm = 0, smOut = 0;
    std::atomic<float> meterIn { -120.0f }, meterOut { -120.0f }, meterRemoved { -120.0f };
    axell::RealFft fft;
    std::array<float, kNfft> win {};
    std::array<Net, kMaxCh> net;
    std::array<std::vector<float>, kMaxCh> inBuf, outAcc, outQ;
    std::vector<float> frameIn, spec, timeOut;
    std::vector<cf> X, Y;
};
} // namespace axdr
