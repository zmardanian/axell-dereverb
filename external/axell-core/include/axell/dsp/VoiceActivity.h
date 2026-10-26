#pragma once
// Axell core - voice activity (from Axell Gate 2.0, identical numerics; framework independent, no JUCE). Shared: Axell
// Gate's detector, Axell Duo Compressor's VOICE HOLD, the Dojo's analysis.
//
// The detector hears the input through a 4th-order Butterworth high-pass at 60 Hz: rumble (stands, traffic, air
// conditioning; often stronger than the room tone above it) carries no voice and would wobble every level the
// decisions rest on. The audio itself is not filtered.
// Every 2 ms hop the shared sibilance detector (axell-core, from Axell Velvet) delivers a frame of voice features. To
// those this adds what a gate needs:
//  - the background floor: minimum statistics of the frame level (the 20 ms-smoothed level's minimum over 1.5 s);
//  - 48 ms of past context of the noisiness, the level above the floor and the treble against the body;
//  - 2.0: the room model (RoomModel.h): how much of the frame the room's own decay cannot explain, how far that
//    unexplained energy is above the room's floor, the room's RT60, and how fast the frame falls re the room's rate;
//  - a 27 -> 32 -> 3 network (VoiceModel.h, trained on these exact inputs: research/train_gate.py) giving the
//    probabilities of background, voice and breath.
// "Loud evidence": a frame within 18 dB of the voice's own running peak level and 12 dB above the floor is voice,
// whatever the network says (no background in a vocal take is that loud).
// Real-time safe: prepare() allocates; push() never allocates, locks or throws.
#include <axell/dsp/Filters.h>
#include <axell/dsp/SibilanceDetector.h>

#include "RoomModel.h"

#include <array>
#include <cmath>
#include <vector>

#ifdef AXELL_VOICE_MODEL_HEADER
 #include AXELL_VOICE_MODEL_HEADER // research builds: held-out models
#else
 #include "VoiceActivityModel.h"
#endif

namespace axell::voice
{
constexpr int kVadInputs = 27;
static_assert (kVadInputs == vamodel::kInputs, "VoiceModel.h does not match the voice-activity inputs");

struct VoiceFrame
{
    int64_t center = 0;          // input sample index of the frame centre
    float x[kVadInputs] {};      // network inputs
    float pBack = 1.0f, pVoice = 0.0f, pBreath = 0.0f;
    float loud = 0.0f;           // P(voice) from level evidence
    float roomTail = 0.0f;       // 2.0: P(the frame is the room ringing out), from the room model
    float levelDb = -140.0f, floorDb = -140.0f, voiceRefDb = -140.0f;
};

class VoiceActivity
{
public:
    static constexpr int kFloorWin = 750;   // frames (1.5 s)
    static constexpr int kCtx = 24;         // frames (48 ms)
    static constexpr double kHpHz = 60.0;   // detection high-pass

    void prepare (double fs)
    {
        hp1.setHighPass (fs, kHpHz, 0.5411961001461970); // Butterworth 4th order: Q = 0.541, 1.307
        hp2.setHighPass (fs, kHpHz, 1.3065629648763766);
        det.prepare (fs);
        room.prepare (fs, det.hopSize());
        reset();
    }

    void reset() noexcept
    {
        det.reset();
        hp1.resetState();
        hp2.resetState();
        room.reset();
        qHead = qTail = 0;
        frame = 0;
        smoothLevel = 0.0f;
        smoothInit = false;
        ctxPos = ctxCount = 0;
        for (auto& c : ctx)
            c.fill (0.0f);
        voiceRef = 0.0f;
        voiceRefInit = false;
    }

    int hopSize() const noexcept { return det.hopSize(); }
    int frameDelay() const noexcept { return det.frameDelay(); }
    const SibilanceDetector& detector() const noexcept { return det; }

    // feed one mono sample; true when out() holds a new frame (centre = newest - frameDelay())
    bool push (float x) noexcept
    {
        const float y = (float) hp2.process (hp1.process (x, 0), 0);
        room.push (y);
        if (! det.push (y))
            return false;
        analyse (det.out());
        return true;
    }

    const VoiceFrame& out() const noexcept { return result; }

    // the network on the inputs: probabilities of background, voice, breath
    static void probabilities (const float* x, float& pb, float& pv, float& pr) noexcept
    {
        float z[kVadInputs];
        for (int i = 0; i < kVadInputs; ++i)
            z[i] = (x[i] - vamodel::kMu[i]) / vamodel::kSd[i];
        float o[3] = { vamodel::kB2[0], vamodel::kB2[1], vamodel::kB2[2] };
        for (int j = 0; j < vamodel::kHidden; ++j)
        {
            float a = vamodel::kB1[j];
            for (int i = 0; i < kVadInputs; ++i)
                a += z[i] * vamodel::kW1[i * vamodel::kHidden + j];
            const float h = std::tanh (a);
            for (int c = 0; c < 3; ++c)
                o[c] += h * vamodel::kW2[j * 3 + c];
        }
        const float m = std::max (o[0], std::max (o[1], o[2]));
        const float e0 = std::exp (o[0] - m), e1 = std::exp (o[1] - m), e2 = std::exp (o[2] - m), s = e0 + e1 + e2;
        pb = e0 / s;
        pv = e1 / s;
        pr = e2 / s;
    }

private:
    // sliding minimum over the last kFloorWin frames (monotone deque in a fixed ring)
    float floorPush (float v) noexcept
    {
        constexpr int R = kFloorWin + 2;
        while (qHead != qTail && qVal[(size_t) ((qTail - 1 + R) % R)] >= v)
            qTail = (qTail - 1 + R) % R;
        qVal[(size_t) qTail] = v;
        qIdx[(size_t) qTail] = frame;
        qTail = (qTail + 1) % R;
        while (qIdx[(size_t) qHead] <= frame - kFloorWin)
            qHead = (qHead + 1) % R;
        return qVal[(size_t) qHead];
    }

    void analyse (const DetFrame& f) noexcept
    {
        VoiceFrame& r = result;
        r.center = f.center;
        const float lev = f.levelDb;
        // the floor: 10-frame smoothing (one pole, 1/10), then the running minimum over 1.5 s. 2.0: digital silence
        // counts as -120 dBFS for it (not -240: the floor would sit there for 1.5 s after the silence before a take)
        const float levF = std::max (lev, -120.0f);
        if (! smoothInit)
        {
            smoothLevel = levF;
            smoothInit = true;
        }
        else
            smoothLevel += (levF - smoothLevel) * 0.1f;
        const float flo = floorPush (smoothLevel);
        ++frame;
        const float reFloor = lev - flo;
        float* x = r.x;
        for (int i = 0; i < kNumFeat; ++i)
            x[i] = f.feat[i];
        for (int i = 3; i < kNumFeat; ++i)
            x[kNumFeat + i - 3] = f.adapted[i];
        x[17] = reFloor;
        x[18] = f.p;
        x[19] = f.apRaw < 0.3f ? 1.0f : 0.0f;
        // past context: aperiodicity, level above the floor, treble against the body
        auto& c = ctx[(size_t) ctxPos];
        c[0] = f.apRaw;
        c[1] = reFloor;
        c[2] = f.feat[9];
        ctxPos = (ctxPos + 1) % kCtx;
        ctxCount = std::min (ctxCount + 1, kCtx);
        for (int i = 0; i < 3; ++i)
        {
            float s = 0.0f;
            for (int k = 0; k < ctxCount; ++k)
                s += ctx[(size_t) k][(size_t) i];
            x[20 + i] = s / (float) ctxCount;
        }
        // the room's verdict on this frame (its centre lies frameDelay samples back)
        const auto rv = room.at ((int) std::lround ((double) det.frameDelay() / det.hopSize()));
        x[23] = rv.excess;
        x[24] = rv.excessDb;
        x[25] = std::log (std::max (0.02f, rv.rt60));
        x[26] = rv.decay;
        // the room veto: a frame the room's decay fully explains and that falls at the room's own rate is the room
        // (a held vowel does not fall, a word start is not explained)
        r.roomTail = (1.0f / (1.0f + std::exp (-(0.12f - rv.excess) / 0.04f))) * (1.0f / (1.0f + std::exp (-(rv.decay - 0.6f) / 0.1f)));
        probabilities (x, r.pBack, r.pVoice, r.pBreath);
        // loud evidence: the voice's own running peak level (frames the network is sure about), falling 1 dB/s
        if (r.pVoice > 0.8f)
        {
            voiceRef = voiceRefInit ? std::max (lev, voiceRef - 0.002f) : lev;
            voiceRefInit = true;
        }
        else if (voiceRefInit)
            voiceRef -= 0.002f;
        auto sig = [] (float z) { return 1.0f / (1.0f + std::exp (-z)); };
        r.loud = voiceRefInit ? sig ((lev - (voiceRef - 18.0f)) / 1.5f) * sig ((lev - flo - 12.0f) / 1.5f) : 0.0f;
        r.levelDb = lev;
        r.floorDb = flo;
        r.voiceRefDb = voiceRefInit ? voiceRef : -140.0f;
    }

    axell::Biquad hp1, hp2;
    SibilanceDetector det;
    RoomModel room;
    VoiceFrame result;
    std::array<float, kFloorWin + 2> qVal {};
    std::array<int64_t, kFloorWin + 2> qIdx {};
    int qHead = 0, qTail = 0;
    int64_t frame = 0;
    float smoothLevel = 0.0f, voiceRef = 0.0f;
    bool smoothInit = false, voiceRefInit = false;
    std::array<std::array<float, 3>, kCtx> ctx {};
    int ctxPos = 0, ctxCount = 0;
};
} // namespace axell::voice
