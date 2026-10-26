#pragma once
// Axell core - the voice / pause hidden Markov model of Axell Gate 2.0 (moved here, identical numerics).
#include <algorithm>
#include <array>

namespace axell::voice
{
//==============================================================================
// hidden Markov model of voice and pause, fixed-lag smoothing: P(voice at t - lag | evidence up to t).
// 2.0: the pause is two hidden states - "just paused" (inside a phrase: a stop's closure, a breath between words; the
// voice is likely to resume within tens of ms) and "true pause" (it rarely resumes) - so the chance of the voice
// resuming depends on how long the pause has lasted (a phase-type approximation of the pause-duration distribution,
// i.e. a hidden semi-Markov model). A word's final release after its closure then reopens the gate on the evidence
// it gives; in a long pause the same evidence is not enough. States: 0 true pause, 1 voice, 2 just paused.
class FixedLagHmm
{
public:
    static constexpr int kMaxLag = 64;
    static constexpr int kS = 3;
    void setup (int lagFrames, double stayVoice, double stayBack, double priorVoice, double resume = 0.0, double toLong = 1.0) noexcept
    {
        lag = std::clamp (lagFrames, 0, kMaxLag - 1);
        for (auto& r : T)
            r.fill (0.0);
        T[1][1] = stayVoice;                   // voice -> voice
        T[1][2] = 1.0 - stayVoice;             // voice -> just paused
        T[2][1] = resume;                      // just paused -> voice
        T[2][0] = toLong;                      // just paused -> true pause
        T[2][2] = std::max (0.0, 1.0 - resume - toLong);
        T[0][0] = stayBack;                    // true pause -> true pause
        T[0][1] = 1.0 - stayBack;              // true pause -> voice
        prior = priorVoice;
    }
    void reset() noexcept
    {
        a = { 0.5, 0.5, 0.0 };
        count = 0;
        pos = 0;
    }
    int lagFrames() const noexcept { return lag; }
    // push the evidence P(voice) of the newest frame; returns false until `lag` frames exist; post = P(voice) of the
    // frame `lag` frames back
    bool push (double pv, double& post) noexcept
    {
        pv = std::clamp (pv, 1.0e-4, 1.0 - 1.0e-4);
        const double lb = (1.0 - pv) / (1.0 - prior), lv = pv / prior;
        const std::array<double, kS> lik { lb, lv, lb };
        std::array<double, kS> na {};
        double s = 0.0;
        for (int j = 0; j < kS; ++j)
        {
            double v = 0.0;
            for (int i = 0; i < kS; ++i)
                v += a[(size_t) i] * T[(size_t) i][(size_t) j];
            na[(size_t) j] = v * lik[(size_t) j];
            s += na[(size_t) j];
        }
        for (int j = 0; j < kS; ++j)
            a[(size_t) j] = na[(size_t) j] / s;
        alpha[(size_t) pos] = a;
        likRing[(size_t) pos] = lik;
        const int cur = pos;
        pos = (pos + 1) % kMaxLag;
        ++count;
        if (count <= lag)
            return false;
        // backward pass over the lag frames
        std::array<double, kS> b { 1.0, 1.0, 1.0 };
        for (int u = 0; u < lag; ++u)
        {
            const auto& l = likRing[(size_t) ((cur - u + kMaxLag) % kMaxLag)];
            std::array<double, kS> nb {};
            double sb = 0.0;
            for (int i = 0; i < kS; ++i)
            {
                double v = 0.0;
                for (int j = 0; j < kS; ++j)
                    v += T[(size_t) i][(size_t) j] * l[(size_t) j] * b[(size_t) j];
                nb[(size_t) i] = v;
                sb += v;
            }
            for (int i = 0; i < kS; ++i)
                b[(size_t) i] = nb[(size_t) i] / sb;
        }
        const auto& at = alpha[(size_t) ((cur - lag + kMaxLag) % kMaxLag)];
        double q = 0.0;
        for (int i = 0; i < kS; ++i)
            q += at[(size_t) i] * b[(size_t) i];
        post = at[1] * b[1] / q;
        return true;
    }

private:
    std::array<std::array<double, kS>, kS> T {};
    double prior = 0.73;
    std::array<double, kS> a { 0.5, 0.5, 0.0 };
    std::array<std::array<double, kS>, kMaxLag> alpha {}, likRing {};
    int lag = 0, count = 0, pos = 0;
};
} // namespace axell::voice
