#pragma once
// Axell core - mastering true-peak limiter (Axell Master). The algorithm of TruePeakLimiter (Axell Duo Compressor
// 2.0, its DECISIONS D21) per channel, with a stereo link amount and a program-dependent release (styles).
//
// Per channel c and base-rate sample b:
//   1. true peak in (b - 1, b + 1): 8x polyphase interpolation (exact to 0.48 fs) + parabolic refinement
//   2. required attenuation a_c = max(1, peak_c / ceiling); linked: a = max over channels; applied in dB:
//      A_c = link * A_linked + (1 - link) * A_c  (>= the channel's own need, so the ceiling holds for every channel)
//   3. running maximum of A_c over the lookahead L -> release (falls instantly, recovers with tau) -> 3 cascaded moving
//      averages with combined support L. Every value entering the averages for sample b is >= its need: no sample and
//      no true peak (content up to 0.48 fs) above the ceiling.
//   Release: tau = tauFast + (tauSlow - tauFast) * m, m = "sustained limiting" memory (charges while the attenuation
//   stays above 1 dB, discharges otherwise): short peaks recover fast (punch, no pumping holes), dense passages recover
//   slowly (no pumping, low bass distortion). Styles set tauFast / tauSlow relative to the RELEASE control and the
//   memory times.
// Latency: 64 + L samples. prepare() allocates; everything else is real-time safe.
#include "Math.h"
#include <cstdint>
#include "SlidingMax.h"
#include "TruePeak.h"

namespace axell
{
class MasterLimiter
{
public:
    static constexpr int kTaps = TruePeakMeter::kTaps, kPhases = TruePeakMeter::kPhases, kSmooth = 3;
    enum class Style { transparent, punchy, loud, safe };
    static constexpr int latencyFor (int look) noexcept { return kTaps / 2 + std::max (kSmooth, look); }

    void prepare (double sampleRate, int maxLookahead)
    {
        fs = sampleRate;
        maxLook = std::max (kSmooth, maxLookahead);
        TruePeakMeter::designTaps (taps);
        int cap = 1;
        while (cap < maxLook + kTaps + 4)
            cap <<= 1;
        mask = cap - 1;
        for (auto& ch : chans)
        {
            ch.delay.assign ((size_t) cap, 0.0f);
            for (auto& r : ch.avgRing)
                r.assign ((size_t) cap, 1.0);
        }
        attMax.prepare (maxLook);
        voiceAvgK = 1.0f - onePole (0.25f, fs);     // VOICE HOLD (process' pause flags): the voice's recent gain, 0.25 s
        pauseGlideK = 1.0f - onePole (0.0125f, fs);  // a pause glides to it in 12.5 ms
        maxHold = (int) std::ceil (kMaxHoldMs * 0.001 * fs);
        holdMax.prepare (maxLook + maxHold);
        setLookahead (std::min (maxLook, (int) std::lround (0.002 * fs)));
        setParams (-1.0f, 120.0f, Style::transparent, 1.0f);
        reset();
    }

    // structural (changes the latency): call while muted, then reset()
    void setLookahead (int samples) noexcept
    {
        look = std::clamp (samples, kSmooth, maxLook);
        attMax.setWindow (look);
        holdMax.setWindow (look + hold);
        int rest = look + kSmooth - 1;
        for (int k = 0; k < kSmooth; ++k)
        {
            avgLen[(size_t) k] = rest / (kSmooth - k);
            rest -= avgLen[(size_t) k];
        }
    }
    int lookahead() const noexcept { return look; }
    int latency() const noexcept { return latencyFor (look); }

    void setParams (float ceilingDb, float releaseMs, Style style, float linkAmount) noexcept
    {
        ceiling = std::pow (10.0f, (std::clamp (ceilingDb, -24.0f, 0.0f) - kMarginDb) / 20.0f);
        const float r = std::clamp (releaseMs, 1.0f, 2000.0f) * 0.001f;
        float fast = r, slow = r, memRise = 0.3f, memFall = 1.0f, holdMs = 15.0f;
        switch (style)
        {
            case Style::transparent: fast = 0.5f * r; slow = 4.0f * r; memRise = 0.25f; memFall = 1.5f; holdMs = 15.0f; break;
            case Style::punchy:      fast = 0.25f * r; slow = 2.5f * r; memRise = 0.5f; memFall = 0.8f; holdMs = 15.0f; break;
            case Style::loud:        fast = 0.3f * r; slow = 0.8f * r; memRise = 0.4f; memFall = 0.6f; holdMs = 15.0f; break;
            case Style::safe:        fast = 1.5f * r; slow = 8.0f * r; memRise = 0.15f; memFall = 3.0f; holdMs = kMaxHoldMs; break;
        }
        // half a period of 33 Hz (25 ms: 20 Hz for Safe); window changes are glitch-free (the max only gets older)
        hold = std::min (maxHold, (int) std::lround (holdMs * 0.001f * (float) fs));
        holdMax.setWindow (look + hold);
        invTauFast = 1.0f / (std::max (0.0005f, fast) * (float) fs);
        invTauSlow = 1.0f / (std::max (0.0005f, slow) * (float) fs);
        memRiseK = 1.0f - onePole (memRise, fs);
        memFallK = 1.0f - onePole (memFall, fs);
        link = std::clamp (linkAmount, 0.0f, 1.0f);
    }

    void reset() noexcept
    {
        for (auto& ch : chans)
        {
            ch.hist.fill (0.0f);
            std::fill (ch.delay.begin(), ch.delay.end(), 0.0f);
            for (int k = 0; k < kSmooth; ++k)
            {
                std::fill (ch.avgRing[(size_t) k].begin(), ch.avgRing[(size_t) k].end(), 1.0);
                ch.avgSum[(size_t) k] = (double) avgLen[(size_t) k];
            }
            ch.prevU = ch.prevPrevU = ch.openPeak = ch.completedPrev = 0.0f;
            ch.env = 1.0f;
            ch.mem = 0.0f;
            ch.gain = 1.0f;
            ch.voiceAvg = 1.0f;
        }
        attMax.resetState();
        holdMax.resetState();
        histPos = pos = 0;
    }

    // in place; returns the deepest gain reduction (dB) of this call. pause (optional, n flags): 1 = the voice pauses
    // (axell-core VoicePresence): VOICE HOLD - the release stops and the gain glides to the voice's recent average, so
    // a pause is not lifted by the release (Axell Duo Compressor 3.0's method). The attack is untouched: the ceiling
    // guarantee holds. Without flags: bit-identical to before.
    float process (float* const* io, int numChannels, int n, const uint8_t* pause = nullptr) noexcept
    {
        const int nc = std::clamp (numChannels, 1, kMaxCh);
        constexpr int centre = kTaps / 2 - 1;
        float minGain = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            float att[kMaxCh] { 1.0f, 1.0f };
            for (int c = 0; c < nc; ++c)
            {
                auto& ch = chans[(size_t) c];
                ch.hist[(size_t) histPos] = ch.hist[(size_t) (histPos + kTaps)] = io[c][i];
                const float* w = ch.hist.data() + histPos + 1;
                float acc[kPhases];
                TruePeakMeter::interpolate (w, taps, acc);
                acc[0] = w[centre];
                float thisPeak = 0.0f, lateForPrev = 0.0f, a = ch.prevPrevU, b = ch.prevU;
                for (int p = 0; p < kPhases; ++p)
                {
                    const float u = std::abs (acc[p]);
                    if (b >= a && b >= u)
                    {
                        const float den = a - 2.0f * b + u;
                        const float pk = den < 0.0f ? b + 0.125f * (a - u) * (a - u) / -den : b;
                        (p == 0 ? lateForPrev : thisPeak) = std::max (p == 0 ? lateForPrev : thisPeak, pk);
                    }
                    thisPeak = std::max (thisPeak, u);
                    a = b;
                    b = u;
                }
                ch.prevPrevU = a;
                ch.prevU = b;
                const float completed = std::max (ch.openPeak, lateForPrev);
                const float peak = std::max (ch.completedPrev, completed);
                ch.completedPrev = completed;
                ch.openPeak = thisPeak;
                att[c] = peak > ceiling ? peak / ceiling : 1.0f;
            }
            histPos = (histPos + 1) % kTaps;

            const float linked = nc > 1 ? std::max (att[0], att[1]) : att[0];
            const int ap = pos & mask;
            for (int c = 0; c < nc; ++c)
            {
                auto& ch = chans[(size_t) c];
                // blend in dB (log domain): exp(link ln L + (1 - link) ln a)
                const float a = link >= 1.0f ? linked : (link <= 0.0f ? att[c] : std::pow (linked, link) * std::pow (att[c], 1.0f - link));
                // bass-aware hold: under sustained limiting (memory charged) the attenuation is held over the look-ahead
                // + hold window, so the gain cannot recover between the peaks of a bass note (no ripple at twice its
                // frequency = no distortion); lone transients (memory empty) release exactly as without it. The held
                // value is never below the required one: the ceiling guarantee is unchanged.
                const float aNow = attMax.process (a, c), aHeld = holdMax.process (a, c);
                const float need = 1.0f / (aNow + (aHeld - aNow) * ch.mem);
                // program-dependent release
                const float tau = invTauFast + (invTauSlow - invTauFast) * ch.mem; // per-sample rate (1 / (tau fs))
                if (pause == nullptr)
                    ch.env = need < ch.env ? need : std::min (need, ch.env + (need - ch.env) * oneMinusExpSmall (std::min (tau, 0.5f)));
                else if (pause[i] == 0)
                {
                    ch.env = need < ch.env ? need : std::min (need, ch.env + (need - ch.env) * oneMinusExpSmall (std::min (tau, 0.5f)));
                    ch.voiceAvg += (ch.env - ch.voiceAvg) * voiceAvgK;
                }
                else
                {
                    if (ch.voiceAvg < ch.env)                       // hold: no release; glide down to the voice's gain
                        ch.env += (ch.voiceAvg - ch.env) * pauseGlideK;
                    ch.env = std::min (need, ch.env);               // the attack as always (the ceiling holds)
                }
                const float w = ch.env < 0.891f ? 1.0f : 0.0f; // sustained limiting above 1 dB charges the memory
                ch.mem += (w - ch.mem) * (w > ch.mem ? memRiseK : memFallK);
                double g = ch.env;
                for (int k = 0; k < kSmooth; ++k)
                {
                    auto& r = ch.avgRing[(size_t) k];
                    const int len = avgLen[(size_t) k];
                    ch.avgSum[(size_t) k] += g - r[(size_t) ((pos - len) & mask)];
                    r[(size_t) ap] = g;
                    g = ch.avgSum[(size_t) k] / (double) len;
                }
                ch.gain = std::min (1.0f, (float) g);
                minGain = std::min (minGain, ch.gain);
                ch.delay[(size_t) ap] = io[c][i];
                io[c][i] = ch.delay[(size_t) ((pos - latency()) & mask)] * ch.gain;
            }
            ++pos;
        }
        return minGain < 1.0f ? -20.0f * std::log10 (minGain) : 0.0f;
    }

    float gainReductionDb (int c) const noexcept { return chans[(size_t) c].gain < 1.0f ? -20.0f * std::log10 (chans[(size_t) c].gain) : 0.0f; }

private:
    // 0.02 dB: > 6x the detector's worst interpolation error up to 0.48 fs (0.003 dB, D21 of Axell Duo Compressor); the
    // input is band-limited to 0.48 fs by the oversampler (Axell Master M2), so that bound applies
    static constexpr float kMarginDb = 0.02f;
    struct Chan
    {
        std::array<float, 2 * kTaps> hist {};
        std::vector<float> delay;
        std::array<std::vector<double>, kSmooth> avgRing;
        std::array<double, kSmooth> avgSum {};
        float prevU = 0, prevPrevU = 0, openPeak = 0, completedPrev = 0, env = 1, mem = 0, gain = 1, voiceAvg = 1;
    };
    double fs = 48000.0;
    int maxLook = 96, look = 96, mask = 0, pos = 0, histPos = 0;
    std::array<int, kSmooth> avgLen {};
    TruePeakMeter::Taps taps {};
    std::array<Chan, kMaxCh> chans {};
    SlidingMax attMax, holdMax;
    static constexpr float kMaxHoldMs = 25.0f;
    int maxHold = 1200, hold = 720;
    float ceiling = 0.891f, invTauFast = 0.0f, invTauSlow = 0.0f, memRiseK = 0.0f, memFallK = 0.0f, link = 1.0f;
    float voiceAvgK = 0.0f, pauseGlideK = 0.0f;
};
} // namespace axell
