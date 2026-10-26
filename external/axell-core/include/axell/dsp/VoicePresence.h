#pragma once
// Axell core - "is a voice sounding right now?" for any plugin of the suite: Axell Gate's whole voice decision, causal.
// VoiceActivity (network + loud evidence, VoiceActivity.h) -> the room veto (what the room's own decay explains is the
// room, not the voice) -> Gate's voice / pause hidden Markov model forward only (FixedLagHmm with no lag) -> a decision
// with Gate's default hold (30 ms). Every 2 ms hop; the decision refers to the frame centre, frameDelay() samples
// behind the newest input (~10 ms: a plugin without look-ahead acts that much late, which a hold covers).
// Used by Axell Duo Compressor's VOICE HOLD (the compressor holds its gain through pauses instead of lifting the room).
// Real-time safe: prepare() allocates; push() never allocates, locks or throws.
#include "FixedLagHmm.h"
#include "VoiceActivity.h"

#include <algorithm>
#include <cmath>

namespace axell::voice
{
class VoicePresence
{
public:
    void prepare (double fs, double holdMs = 30.0)
    {
        va.prepare (fs);
        hop = va.hopSize();
        holdFrames = std::max (0, (int) std::lround (holdMs * 0.001 * fs / hop));
        roomRunFrames = std::max (1, (int) std::lround (0.030 * fs / hop));
        reset();
    }

    void reset() noexcept
    {
        va.reset();
        hmm.setup (0, 0.998, 0.995, (double) vamodel::kPriorVoice, 0.03, 0.012);
        hmm.reset();
        roomTailSm = 0.0f;
        roomRun = 0;
        held = 0;
        post = 0.0;
        isVoice = false;
    }

    int hopSize() const noexcept { return hop; }
    int frameDelay() const noexcept { return va.frameDelay(); }

    // one mono sample; true when a new decision was made (every hop)
    bool push (float x) noexcept
    {
        if (! va.push (x))
            return false;
        const auto& f = va.out();
        roomTailSm += 0.2f * (f.roomTail - roomTailSm);
        roomRun = roomTailSm > 0.5f ? roomRun + 1 : 0;
        const double veto = roomRun >= roomRunFrames ? 1.0 : 0.0;
        const double pv = std::max ((double) f.pVoice, (double) f.loud) * (1.0 - veto);
        double p = 0.0;
        if (hmm.push (pv, p))
            post = p;
        if (post > kOpen)
        {
            isVoice = true;
            held = holdFrames;
        }
        else if (held > 0)
            --held;
        else
            isVoice = false;
        return true;
    }

    bool voice() const noexcept { return isVoice; }     // the decision (with the hold)
    double probability() const noexcept { return post; } // P(voice) after the pause model
    const VoiceActivity& activity() const noexcept { return va; }

    static constexpr double kOpen = 0.3;                 // Axell Gate's default open threshold (SENSITIVITY 0)

private:
    VoiceActivity va;
    FixedLagHmm hmm;
    int hop = 96, holdFrames = 15, roomRunFrames = 15, roomRun = 0, held = 0;
    float roomTailSm = 0.0f;
    double post = 0.0;
    bool isVoice = false;
};
} // namespace axell::voice
