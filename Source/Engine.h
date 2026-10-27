#pragma once
// Axell De-Reverb - the engine (framework independent, no JUCE): streaming STFT (1024 / 512, sqrt-Hann analysis and
// synthesis, 50 % overlap-add), the network per frame, inverse STFT. Mono detection path; each channel gets its own
// network instance (a stereo vocal stays stereo). Latency: one frame (1024) + LA hops of look-ahead.
// 3.0 LIVE (weights with LA = 0, a causal network): no latency. Each hop the network's frame becomes a magnitude
// response (|enhanced| / |input| per bin) and that a minimum-phase filter (axell-core MinPhase.h, Velvet 3.0 LIVE's
// method), which filters the current input sample by sample and fades into the next one over a hop.
// REDUCTION (0..1) mixes the processed signal with the latency-aligned input here, at 48 kHz, so dry and processed
// paths stay sample-aligned whatever the host rate; LISTEN plays what is removed (input - processed) x REDUCTION.
// Real-time safe after prepare(): no allocation, locks or I/O in process().
#include "TrueTone.h"
#include <axell/dsp/Fft.h>
#include <axell/dsp/MinPhase.h>

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
        live = w.la == 0;
        lat = live ? 0 : kNfft + w.la * kHop;
        fft.prepare (kNfft);
        if (live)
        {
            design.prepare (kNfft, kLiveTaps);
            ratio.assign ((size_t) kNb, 1.0f);
            hDesign.assign ((size_t) kLiveTaps, 0.0f);
            for (auto& f : fir)
                f.prepare (kLiveTaps, kHop);
        }
        for (int i = 0; i < kNfft; ++i)
            win[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / kNfft));
        for (int c = 0; c < nch; ++c)
        {
            net[(size_t) c].prepare (w);
            inBuf[(size_t) c].assign (kNfft, 0.0f);
            outAcc[(size_t) c].assign (kNfft, 0.0f);
            outQ[(size_t) c].assign (kHop, 0.0f);
            dry[(size_t) c].assign ((size_t) std::max (1, latency()), 0.0f);
        }
        frameIn.assign (kNfft, 0.0f);
        spec.assign (kNfft + 2, 0.0f);
        X.assign (kNb, cf {});
        Y.assign (kNb, cf {});
        timeOut.assign (kNfft, 0.0f);
        tone.prepare (kNb, kNfft, (double) kFs);
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
            fir[(size_t) c].reset();
            onsetEnv[(size_t) c] = onsetW[(size_t) c] = 0.0f;
            framePow[(size_t) c] = 1.0e30f;                // no frame yet: no guard
        }
        fill = 0;
        dryPos = 0;
        accIn = accRm = accOut = smIn = smRm = smOut = 0.0;
        tone.reset();
        amt = amtTarget;
        lis = lisTarget;
    }

    // input sample n comes out at sample n + latency() (0 in LIVE)
    int latency() const noexcept { return lat; }
    bool isLive() const noexcept { return live; }
    static constexpr int studioLatency() noexcept { return kNfft + kLa * kHop; }

    void setAmount (float a) noexcept { amtTarget = std::clamp (a, 0.0f, 1.0f); }
    // meters (any thread): input / output level (dBFS, ~0.5 s) and the removed room re the input (dB)
    float inputDb() const noexcept { return meterIn.load (std::memory_order_relaxed); }
    float outputDb() const noexcept { return meterOut.load (std::memory_order_relaxed); }
    float removedDb() const noexcept { return meterRemoved.load (std::memory_order_relaxed); }
    void setListen (bool on) noexcept { lisTarget = on ? 1.0f : 0.0f; }
    // 4.0 TRUE TONE (TrueTone.h): off = the 3.x path, sample for sample
    void setTrueTone (bool on) noexcept { trueTone = on; }
    // research: measure as TRUE TONE does but leave the sound untouched (the model's features, exactly as the plugin sees them)
    void setTrueToneMeasureOnly (bool on) noexcept { trueToneMeasureOnly = on; }
    float trueToneDb (int band) const noexcept { return tone.curveDb (band); }
    float trueToneMeasuredDb (int band) const noexcept { return tone.measuredDb (band); }

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
                float dr = x, wet = 0.0f;
                if (live)
                {
                    wet = fir[(size_t) c].process (x);     // the current sample, filtered: no delay
                    // onset guard: a room only decays, so a level well above the last analysed frame's is direct sound
                    // the filter (from that frame) has not seen - let it through until the next frames catch up
                    auto& e = onsetEnv[(size_t) c];
                    e += (x * x - e) * onsetEnvK;
                    auto& w = onsetW[(size_t) c];
                    const float tgt = e > onsetRatio * framePow[(size_t) c] ? 1.0f : 0.0f;
                    w += (tgt - w) * (tgt > w ? onsetAtkK : onsetRelK);
                    wet += w * (x - wet);
                }
                else
                {
                    auto& d = dry[(size_t) c];
                    dr = d[(size_t) dryPos];               // the input, latency() samples ago
                    d[(size_t) dryPos] = x;
                    wet = outQ[(size_t) c][(size_t) fill];
                }
                const float removed = dr - wet;
                const float mixed = dr - amt * removed;     // REDUCTION
                s = mixed + lis * (amt * removed - mixed);  // LISTEN: what is removed
                accIn += (double) dr * dr;
                accRm += (double) (amt * removed) * (amt * removed);
                accOut += (double) s * s;
            }
            if (! live)
                dryPos = (dryPos + 1) % latency();
            if (++fill == kHop)
            {
                fill = 0;
                for (int c = 0; c < ch; ++c)
                    hop (c);
                if (trueTone)
                    tone.update (kNb, amt);
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
        if (trueTone)
        {
            tone.observe (X.data(), Y.data(), kNb);   // what the network does, before the correction
            if (! live && ! trueToneMeasureOnly)
                for (int k = 0; k < kNb; ++k)
                    Y[(size_t) k] *= tone.binGain (k);
        }
        if (live)
        {
            liveFilter (c);
            std::copy (ib.begin() + kHop, ib.end(), ib.begin());
            return;
        }
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

    // LIVE: this frame's magnitude response -> the next minimum-phase filter (unchanged in digital silence: the network
    // passes nothing there, which is no response)
    void liveFilter (int c) noexcept
    {
        double pm = 0.0;
        for (int k = 0; k < kNb; ++k)
            pm += (double) std::norm (X[(size_t) k]);
        // the frame's mean power per sample (sqrt-Hann window: sum w^2 = N/2; Parseval: sum |X|^2 over the full
        // spectrum = N sum (x w)^2, the one-sided sum counts it about half)
        framePow[(size_t) c] = (float) (2.0 * pm / ((double) kNfft * (double) kNfft * 0.5));
        if (pm / kNb < kLiveSilentPower)
            return;
        for (int k = 0; k < kNb; ++k)
        {
            const float px = std::norm (X[(size_t) k]);
            // bins far below the frame's level carry no measurable response: they keep the neighbours' (1 = untouched)
            const float r = px > 1.0e-9f * (float) (pm / kNb) ? std::sqrt (std::norm (Y[(size_t) k]) / px) : 1.0f;
            ratio[(size_t) k] = std::clamp (r, kLiveFloor, 1.0f);
            if (trueTone && ! trueToneMeasureOnly)   // 4.0: the tone correction on top of the removal (up to +3 dB back)
                ratio[(size_t) k] *= tone.binGain (k);
        }
        design.design (ratio.data(), hDesign.data());
        fir[(size_t) c].setFilter (hDesign.data());
    }

#ifndef AXDR_ONSET_DB
 #define AXDR_ONSET_DB 8.0f
#endif
#ifndef AXDR_ONSET_REL_MS
 #define AXDR_ONSET_REL_MS 25.0f
#endif
    std::array<float, kMaxCh> onsetEnv {}, onsetW {}, framePow {};
    float onsetRatio = std::pow (10.0f, AXDR_ONSET_DB / 10.0f);
    float onsetEnvK = 1.0f - std::exp (-1.0f / (0.002f * (float) kFs));
    float onsetAtkK = 1.0f - std::exp (-1.0f / (0.001f * (float) kFs));
    float onsetRelK = 1.0f - std::exp (-1.0f / (AXDR_ONSET_REL_MS * 0.001f * (float) kFs));
    static constexpr int kLiveTaps = 512;              // 10.7 ms of response; the design grid is the STFT's (513 bins)
    static constexpr float kLiveFloor = 0.0316f;       // -30 dB: never more removal per bin than that
    static constexpr double kLiveSilentPower = 512.0 * 1.0e-13;
    static constexpr float kSmooth = 0.0005f; // ~40 ms at 48 kHz
    int nch = 1, fill = 0, dryPos = 0, lat = studioLatency();
    bool live = false, trueTone = false, trueToneMeasureOnly = false;
    TrueTone tone;
    axell::MinPhaseDesigner design;
    std::array<axell::MinPhaseFir, kMaxCh> fir;
    std::vector<float> ratio, hDesign;
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
