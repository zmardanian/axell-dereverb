// Axell De-Reverb - plugin-level tests (JUCE): parameters, state, presets, A/B, latency per rate, host bypass, mono,
// sample-rate changes, editor. Exit code = number of failures.
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <cstdio>
#include <random>

namespace
{
int failures = 0, checks = 0;
void check (bool ok, const juce::String& what)
{
    ++checks;
    if (! ok)
        ++failures;
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
}
void setParam (DeReverbProcessor& p, const char* id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}
float getParam (DeReverbProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }
void prepare (DeReverbProcessor& p, double fs = 48000.0, int block = 256, int ch = 2)
{
    p.setPlayConfigDetails (ch, ch, fs, block);
    p.prepareToPlay (fs, block);
}
void pumpMessages (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

std::vector<std::vector<float>> render (DeReverbProcessor& p, std::vector<std::vector<float>> x, int block, bool bypassed = false)
{
    const int nc = (int) x.size();
    juce::AudioBuffer<float> buf (nc, block);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos < x[0].size(); pos += (size_t) block)
    {
        const int m = (int) std::min<size_t> ((size_t) block, x[0].size() - pos); // the last block may be shorter
        buf.setSize (nc, m, false, false, true);
        for (int c = 0; c < nc; ++c)
            buf.copyFrom (c, 0, x[(size_t) c].data() + pos, m);
        if (bypassed)
            p.processBlockBypassed (buf, midi);
        else
            p.processBlock (buf, midi);
        for (int c = 0; c < nc; ++c)
            std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + m, x[(size_t) c].begin() + (long) pos);
    }
    return x;
}
std::vector<float> voiceLike (int n, double fs, unsigned seed)
{
    std::vector<float> x ((size_t) n);
    std::mt19937 g (seed);
    std::normal_distribution<float> nd;
    double ph = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, u = std::fmod (t, 0.5);
        ph += 2 * juce::MathConstants<double>::pi * (130 + 20 * std::sin (t)) / fs;
        double y = 0;
        for (int h = 1; h < 25; ++h)
            y += std::sin (h * ph) / h;
        x[(size_t) i] = (float) ((u < 0.35 ? 0.15 * y : 0.0) + 0.001 * nd (g));
    }
    return x;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    //== 1. parameters: frozen IDs and order, defaults; the embedded weights load
    {
        DeReverbProcessor p;
        const juce::StringArray frozen { "reduction", "listen", "output", "mode", "trueTone" };   // "mode": 3.0 (hint 2), "trueTone": 4.0 (hint 4)
        auto& params = p.getParameters();
        check (params.size() == frozen.size(), "parameter count = 5");
        for (int i = 0; i < params.size() && i < frozen.size(); ++i)
        {
            auto* wp = dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i]);
            check (wp != nullptr && wp->getParameterID() == frozen[i] && wp->getVersionHint() == (i < 3 ? 1 : (i == 3 ? 2 : 4)),
                   "parameter " + juce::String (i) + " is '" + frozen[i] + "'");
        }
        check (getParam (p, "mode") < 0.5f, "default mode: STUDIO (projects from before 3.0 load as they were)");
        check (std::abs (getParam (p, "reduction") - 85.0f) < 1e-4f && getParam (p, "listen") < 0.5f && std::abs (getParam (p, "output")) < 1e-4f,
               "defaults: REDUCTION 85 %, LISTEN off, OUTPUT 0 dB (= the Natural preset)");
        check (p.weightsOk(), "the embedded network weights load and verify");
    }

    //== 2. state round trip
    {
        DeReverbProcessor a;
        setParam (a, "reduction", 63.0f);
        setParam (a, "listen", 1.0f);
        setParam (a, "output", -2.5f);
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        DeReverbProcessor b;
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        check (std::abs (getParam (b, "reduction") - 63.0f) < 1e-3f && getParam (b, "listen") > 0.5f && std::abs (getParam (b, "output") + 2.5f) < 1e-3f,
               "state restore: all parameters identical");
    }

    //== 2b. 4.0 TRUE TONE: on for new instances, off for projects saved before 4.0, kept as saved since
    {
        DeReverbProcessor fresh;
        check (getParam (fresh, "trueTone") > 0.5f, "4.0: a new instance has TRUE TONE on");
        {
            DeReverbProcessor q;
            setParam (q, "trueTone", 0.0f);
            bool on = true;
            for (int i = 0; i < q.getNumPrograms(); ++i)
            {
                setParam (q, "trueTone", 0.0f);
                q.setCurrentProgram (i);
                on = on && getParam (q, "trueTone") > 0.5f;
            }
            check (on, "4.0: every factory preset turns TRUE TONE on");
        }
        auto stateWith = [] (int how) {   // 0: no trueTone (a 3.x project), 1: trueTone without a value
            DeReverbProcessor a;
            setParam (a, "reduction", 100.0f);
            auto tree = a.apvts.copyState();
            for (int i = tree.getNumChildren(); --i >= 0;)
                if (tree.getChild (i).getProperty ("id").toString() == "trueTone")
                {
                    if (how == 0)
                        tree.removeChild (i, nullptr);
                    else
                        tree.getChild (i).removeProperty ("value", nullptr);
                }
            juce::MemoryBlock mb;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), mb);
            return mb;
        };
        for (int how : { 0, 1 })
        {
            const auto mb = stateWith (how);
            DeReverbProcessor b;
            b.setStateInformation (mb.getData(), (int) mb.getSize());
            check (getParam (b, "trueTone") < 0.5f && std::abs (getParam (b, "reduction") - 100.0f) < 1e-3f,
                   how == 0 ? "4.0: a 3.x project (no TRUE TONE in it) loads with it off, the rest as saved"
                            : "4.0: a state with a value-less TRUE TONE entry loads with it off");
        }
        for (const float v : { 0.0f, 1.0f })
        {
            DeReverbProcessor a;
            setParam (a, "trueTone", v);
            juce::MemoryBlock mb;
            a.getStateInformation (mb);
            DeReverbProcessor b;
            b.setStateInformation (mb.getData(), (int) mb.getSize());
            check ((getParam (b, "trueTone") > 0.5f) == (v > 0.5f), juce::String ("4.0: a 4.0 project keeps TRUE TONE ") + (v > 0.5f ? "on" : "off"));
        }
    }

    //== 3. presets and A/B
    {
        DeReverbProcessor p;
        bool ok = true;
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            p.setCurrentProgram (i);
            ok = ok && std::abs (getParam (p, "reduction") - DeReverbProcessor::factoryPresets()[(size_t) i].reduction) < 0.05f && getParam (p, "listen") < 0.5f;
        }
        check (ok && p.getNumPrograms() == 3, "every factory preset applies (" + juce::String (p.getNumPrograms()) + ")");
        p.setCurrentProgram (0);
        setParam (p, "reduction", 30.0f);
        p.selectSlot (1);
        setParam (p, "reduction", 80.0f);
        p.selectSlot (0);
        const float a = getParam (p, "reduction");
        p.selectSlot (1);
        const float b = getParam (p, "reduction");
        check (std::abs (a - 30.0f) < 1e-3f && std::abs (b - 80.0f) < 1e-3f, "A/B: each slot keeps its own settings");
    }

    //== 3b. LIVE: no latency at 48 kHz; STUDIO <-> LIVE without a host switches at once and reports the new latency
    for (double fs : { 44100.0, 48000.0 })
    {
        DeReverbProcessor p;
        setParam (p, "mode", 1.0f);
        prepare (p, fs, 256);
        const int L = p.getLatencySamples();
        const auto x = voiceLike ((int) fs * 2, fs, 3);
        setParam (p, "reduction", 0.0f);
        p.reset();
        const auto y0 = render (p, { x, x }, 256);
        double e = 0, r = 0;
        for (size_t i = (size_t) fs / 2; i + (size_t) fs / 8 < x.size(); ++i)
        {
            e += (double) (y0[0][i] - x[i - (size_t) L]) * (y0[0][i] - x[i - (size_t) L]);
            r += (double) x[i - (size_t) L] * x[i - (size_t) L];
        }
        check (p.liveActive() && (fs == 48000.0 ? L == 0 : L < fs * 0.004) && 10 * std::log10 (e / r + 1e-30) < -120,
               "LIVE @ " + juce::String (fs / 1000.0, 1) + " kHz: latency " + juce::String (L) + " samples; REDUCTION 0 = the input ("
                   + juce::String (10 * std::log10 (e / r + 1e-30), 1) + " dB)");
        setParam (p, "mode", 0.0f);
        setParam (p, "reduction", 85.0f);
        const std::vector<float> shortX (x.begin(), x.begin() + (long) (0.2 * fs));   // 0.2 s per step, as a host
        for (int k = 0; k < 4; ++k)                                                     // with a live message thread
        {
            render (p, { shortX, shortX }, 256);   // the audio thread fades out and asks for the new latency
            pumpMessages (60);           // the 20 Hz timer reports it to the host; the next blocks play it
        }
        check (! p.liveActive() && p.getLatencySamples() > L, "LIVE -> STUDIO through the latency handshake: switched, latency "
                                                               + juce::String (p.getLatencySamples()));
    }

    //== 4. latency per rate, reported to the host; REDUCTION 0 transparent; host bypass = the input delayed exactly
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        DeReverbProcessor p;
        prepare (p, fs, 256);
        const int L = p.getLatencySamples();
        const auto x = voiceLike ((int) fs * 2, fs, 3);
        setParam (p, "reduction", 0.0f);
        p.reset();
        const auto y0 = render (p, { x, x }, 256);
        double e = 0, r = 0;
        for (size_t i = (size_t) fs / 2; i + (size_t) fs / 8 < x.size(); ++i)
        {
            e += (double) (y0[0][i] - x[i - (size_t) L]) * (y0[0][i] - x[i - (size_t) L]);
            r += (double) x[i - (size_t) L] * x[i - (size_t) L];
        }
        check (L > 0 && L < fs * 0.07 && 10 * std::log10 (e / r + 1e-30) < -120,
               "@ " + juce::String (fs / 1000.0, 1) + " kHz: latency " + juce::String (L) + " samples (" + juce::String (1000.0 * L / fs, 1)
                   + " ms); REDUCTION 0 = the input delayed by it (" + juce::String (10 * std::log10 (e / r + 1e-30), 1) + " dB)");
        DeReverbProcessor q;
        prepare (q, fs, 256);
        const auto yb = render (q, { x, x }, 256, true);
        double eb = 0;
        for (size_t i = (size_t) L + 2048; i < x.size(); ++i)
            eb = std::max (eb, (double) std::abs (yb[0][i] - x[i - (size_t) L]));
        check (eb == 0.0, "@ " + juce::String (fs / 1000.0, 1) + " kHz: host bypass = exactly the input delayed by the latency (max error "
                              + juce::String (eb) + ")");
    }

    //== 5. mono, sample-rate change, a host block larger than announced
    {
        DeReverbProcessor p;
        auto mono = p.getBusesLayout();
        mono.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        mono.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
        check (p.setBusesLayout (mono), "mono layout accepted");
        p.setRateAndBufferSizeDetails (44100.0, 300);
        p.prepareToPlay (44100.0, 300);
        auto y = render (p, { voiceLike (44100, 44100.0, 4) }, 300);
        bool fin = true;
        for (float v : y[0])
            fin = fin && std::isfinite (v);
        p.prepareToPlay (96000.0, 128);                          // a rate change between runs
        auto y2 = render (p, { voiceLike (96000, 96000.0, 5) }, 1024); // blocks larger than announced
        for (float v : y2[0])
            fin = fin && std::isfinite (v);
        check (fin, "mono: processes; then a rate change and blocks larger than announced: finite");
    }

    //== 6. editor: open/close while processing; size limits
    {
        DeReverbProcessor p;
        prepare (p);
        bool ok = true;
        for (int i = 0; i < 5; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            render (p, { voiceLike (4096, 48000.0, 6), voiceLike (4096, 48000.0, 7) }, 256);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
            ok = ok && ed->getWidth() == gui::kWidth;
        }
        check (ok, "editor: opens/closes 5x while processing (100 % size)");
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* c = ed->getConstrainer();
        check (c != nullptr && c->getMinimumWidth() == gui::kWidth * 3 / 4 && c->getMaximumWidth() == 2 * gui::kWidth, "editor: resizable 75-200 %");
    }

    std::printf ("%s: %d of %d plugin checks passed\n", failures == 0 ? "ALL PASS" : "FAILURES", checks - failures, checks);
    return failures;
}
