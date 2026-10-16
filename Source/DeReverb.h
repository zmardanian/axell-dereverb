#pragma once
// Axell De-Reverb - the complete DSP chain at any host rate (framework independent, no JUCE):
//   host rate -> Resampler -> 48 kHz Engine (network, REDUCTION, LISTEN) -> Resampler -> host rate.
// At 48 kHz the resamplers are bypassed. A small primed output FIFO keeps the latency constant whatever the host's
// block sizes (the two rational resamplers produce a varying number of samples per block, exactly the right number on
// average). latencySamples() is the total delay in host samples, rounded; the sub-sample remainder (resampling only)
// is removed by the output resampler's start phase, so the total is a whole number of samples.
// Band above the network (44.1 / 88.2 / 96 kHz hosts): the resamplers keep 0 .. ~20.7 kHz (44.1) or ~22.6 kHz. The
// input also takes the same round trip without processing; (input delayed) - (its round trip) is exactly the band
// the round trip loses, and it is added back: at REDUCTION 0 the output is the input, bit-transparent within float
// precision, at every rate; above the network's band the voice passes untouched.
// Real-time safe after prepare().
#include "Engine.h"
#include "Resampler.h"

namespace axdr
{
class DeReverb
{
public:
    static constexpr int kPrime = 64;   // host samples of output FIFO priming

    bool prepare (const Weights& w, double hostRate, int numChannels, int maxBlock)
    {
        fsHost = (int) std::lround (hostRate);
        nch = std::clamp (numChannels, 1, Engine::kMaxCh);
        maxB = std::max (1, maxBlock);
        if (! engine.prepare (w, nch))
            return false;
        resampled = fsHost != kFs;
        const int max48 = (int) std::ceil ((double) maxB * kFs / fsHost) + 64;
        for (int c = 0; c < nch; ++c)
        {
            up[(size_t) c].prepare (fsHost, kFs, maxB);
            down[(size_t) c].prepare (kFs, fsHost, max48);
            dryDown[(size_t) c].prepare (kFs, fsHost, max48);    // before setExtraDelay (its units depend on L)
        }
        if (resampled)
        {
            // make the total delay a whole number of host samples: the output resampler adds the missing fraction
            const double d = up[0].delayIn() + (Engine::latency() + down[0].delayIn()) * fsHost / (double) kFs + kPrime;
            const double add48 = (std::ceil (d - 1.0e-9) - d) * kFs / fsHost;
            for (int c = 0; c < nch; ++c)
            {
                down[(size_t) c].setExtraDelay (add48);
                dryDown[(size_t) c].setExtraDelay (add48);
            }
        }
        for (int c = 0; c < nch; ++c)
        {
            buf48[(size_t) c].assign ((size_t) max48, 0.0f);
            dry48[(size_t) c].assign ((size_t) max48, 0.0f);
            dryFifo[(size_t) c].assign ((size_t) (kPrime + 2 * maxB + 512), 0.0f);
            dryRing48[(size_t) c].assign ((size_t) Engine::latency(), 0.0f);
            outFifo[(size_t) c].assign ((size_t) (kPrime + 2 * maxB + 512), 0.0f);
            tmpOut[(size_t) c].assign ((size_t) (maxB + 512), 0.0f);
        }
        reset();
        return true;
    }

    void reset() noexcept
    {
        engine.reset();
        for (int c = 0; c < nch; ++c)
        {
            up[(size_t) c].reset();
            down[(size_t) c].reset();
            dryDown[(size_t) c].reset();
            std::fill (outFifo[(size_t) c].begin(), outFifo[(size_t) c].end(), 0.0f);
            std::fill (dryFifo[(size_t) c].begin(), dryFifo[(size_t) c].end(), 0.0f);
            std::fill (dryRing48[(size_t) c].begin(), dryRing48[(size_t) c].end(), 0.0f);
            hostRing[(size_t) c].assign ((size_t) std::max (1, latencySamples()) + 1, 0.0f);
        }
        fifoLen = resampled ? kPrime : 0;
        ring48Pos = 0;
        hostPos = 0;
        hb = 1.0f - hbListen;
    }

    // total latency in host samples
    int latencySamples() const noexcept
    {
        if (! resampled)
            return Engine::latency();
        const double d = up[0].delayIn() + (Engine::latency() + down[0].delayIn()) * fsHost / (double) kFs + kPrime;
        return (int) std::lround (d);
    }

    void setAmount (float a) noexcept { engine.setAmount (a); }
    void setListen (bool on) noexcept
    {
        engine.setListen (on);
        hbListen = on ? 1.0f : 0.0f;
    }

    void process (float* const* io, int numChannels, int n) noexcept
    {
        const int ch = std::min (numChannels, nch);
        // clean the input once, before any filter state can be poisoned by a NaN / Inf
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < n; ++i)
                io[c][i] = std::isfinite (io[c][i]) ? std::clamp (io[c][i], -1000.0f, 1000.0f) : 0.0f;
        if (! resampled)
        {
            engine.process (io, ch, n);
            return;
        }
        // to 48 kHz
        int m48 = 0;
        float* p48[Engine::kMaxCh] {};
        for (int c = 0; c < ch; ++c)
        {
            m48 = up[(size_t) c].process (io[c], n, buf48[(size_t) c].data(), (int) buf48[(size_t) c].size());
            p48[c] = buf48[(size_t) c].data();
        }
        // the unprocessed copy at 48 kHz, delayed like the engine's output
        const int L48 = Engine::latency();
        for (int c = 0; c < ch; ++c)
        {
            auto& r = dryRing48[(size_t) c];
            auto& d = dry48[(size_t) c];
            int pos = ring48Pos;
            for (int i = 0; i < m48; ++i)
            {
                const float v = p48[c][i];
                d[(size_t) i] = r[(size_t) pos];
                r[(size_t) pos] = v;
                pos = (pos + 1) % L48;
            }
        }
        ring48Pos = (ring48Pos + m48) % L48;
        engine.process (p48, ch, m48);
        // both back to the host rate, into the FIFOs; then exactly n samples out
        int got = 0;
        for (int c = 0; c < ch; ++c)
        {
            got = down[(size_t) c].process (p48[c], m48, tmpOut[(size_t) c].data(), (int) tmpOut[(size_t) c].size());
            auto& f = outFifo[(size_t) c];
            const int room = (int) f.size() - fifoLen;
            std::copy (tmpOut[(size_t) c].begin(), tmpOut[(size_t) c].begin() + std::min (got, room), f.begin() + fifoLen);
            const int gotD = dryDown[(size_t) c].process (dry48[(size_t) c].data(), m48, tmpOut[(size_t) c].data(), (int) tmpOut[(size_t) c].size());
            auto& fd = dryFifo[(size_t) c];
            std::copy (tmpOut[(size_t) c].begin(), tmpOut[(size_t) c].begin() + std::min (gotD, room), fd.begin() + fifoLen);
        }
        fifoLen = std::min ((int) outFifo[0].size(), fifoLen + got);
        const int take = std::min (n, fifoLen);
        const int Lh = (int) hostRing[0].size();
        for (int c = 0; c < ch; ++c)
        {
            auto& f = outFifo[(size_t) c];
            auto& fd = dryFifo[(size_t) c];
            auto& hr = hostRing[(size_t) c];
            int hp = hostPos;
            float hbc = hb;
            for (int i = 0; i < n; ++i)
            {
                const float x = std::isfinite (io[c][i]) ? std::clamp (io[c][i], -1000.0f, 1000.0f) : 0.0f;
                const int rd = (hp + 1) % Lh;              // written exactly latencySamples() ago
                hr[(size_t) hp] = x;
                const float dryDelayed = hr[(size_t) rd];
                hp = rd;
                hbc += ((1.0f - hbListen) - hbc) * 0.0005f;
                const float y = i < take ? f[(size_t) i] + hbc * (dryDelayed - fd[(size_t) i]) : 0.0f;
                io[c][i] = y;
            }
            if (c == ch - 1)
            {
                hostPos = hp;
                hb = hbc;
            }
            std::copy (f.begin() + take, f.begin() + fifoLen, f.begin());
            std::copy (fd.begin() + take, fd.begin() + fifoLen, fd.begin());
        }
        fifoLen -= take;
        underruns += take < n ? 1 : 0;
    }

    int underrunCount() const noexcept { return underruns; }
    const Engine& core() const noexcept { return engine; }

private:
    Engine engine;
    std::array<Resampler, Engine::kMaxCh> up, down;
    std::array<std::vector<float>, Engine::kMaxCh> buf48, outFifo, tmpOut, dry48, dryFifo, dryRing48, hostRing;
    std::array<Resampler, Engine::kMaxCh> dryDown;
    int ring48Pos = 0, hostPos = 0;
    float hb = 1.0f, hbListen = 0.0f;
    int fsHost = kFs, nch = 1, maxB = 512, fifoLen = 0, underruns = 0;
    bool resampled = false;
};
} // namespace axdr
