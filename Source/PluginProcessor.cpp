#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

using APVTS = juce::AudioProcessorValueTreeState;

namespace
{
juce::AudioParameterFloatAttributes fmt (std::function<juce::String (float)> toText)
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([toText] (float v, int) { return toText (v); })
        .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue(); });
}
juce::String dbText (float v) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String pctText (float v) { return juce::String (juce::roundToInt (v)) + " %"; }
} // namespace

APVTS::ParameterLayout DeReverbProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    // IDs and order are frozen from 1.0 on (saved projects, automation); new parameters are appended
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "reduction", 1 }, "Reduction",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 85.0f, fmt (pctText)));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "listen", 1 }, "Listen: Removed", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, fmt (dbText)));
    // 3.0: STUDIO (the look-ahead network, 53 ms) or LIVE (a causal network, no latency: for tracking). Projects saved
    // before 3.0 load in STUDIO (the default), as they were.
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 2 }, "Mode", StringArray { "Studio", "Live" }, 0));
    return { p.begin(), p.end() };
}

DeReverbProcessor::DeReverbProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    weights.load (BinaryData::dereverb_bin, (size_t) BinaryData::dereverb_binSize);
    weightsLive.load (BinaryData::dereverb_live_bin, (size_t) BinaryData::dereverb_live_binSize);
    jassert (weights.la == axdr::kLa && weightsLive.la == 0);
    pReduction = apvts.getRawParameterValue ("reduction");
    pListen = apvts.getRawParameterValue ("listen");
    pOutput = apvts.getRawParameterValue ("output");
    pMode = apvts.getRawParameterValue ("mode");
    startTimerHz (20);
}

bool DeReverbProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) && out == layouts.getMainInputChannelSet();
}

void DeReverbProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedChannels = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    const int maxBlock = juce::jmax (samplesPerBlock, 32);
    maxChunk = maxBlock;
    for (auto& b : inCopy)
        b.assign ((size_t) maxBlock, 0.0f);
    dr.prepare (weights, sampleRate, preparedChannels, maxBlock);
    drLive.prepare (weightsLive, sampleRate, preparedChannels, maxBlock);
    activeMode = pMode->load() > 0.5f ? 1 : 0;
    latency = latencyOf (activeMode);
    setLatencySamples (latency);
    hs.prepared (latency, sampleRate);
    hs.setNonRealtime (isNonRealtime());
    hs.setHostLatency (getLatencySamples());
    for (auto& d : dryLine)
        d.assign ((size_t) juce::jmax (dr.latencySamples(), drLive.latencySamples()) + 1, 0.0f);
    dryPos = 0;
    bypassStep = (float) (1.0 / juce::jmax (1.0, 0.01 * sampleRate));
    fadeStep = (float) (1.0 / juce::jmax (1.0, 0.003 * sampleRate));
    fade = 1.0f;
    warm = 0;
    reset();
}

void DeReverbProcessor::reset()
{
    if (preparedChannels == 0)
        return;
    dr.reset();
    drLive.reset();
    for (auto& d : dryLine)
        std::fill (d.begin(), d.end(), 0.0f);
    dryPos = 0;
}

void DeReverbProcessor::run (juce::AudioBuffer<float>& buffer, bool bypass)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int nch = juce::jmin (buffer.getNumChannels(), preparedChannels);
    for (int c = getMainBusNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, n);
    if (preparedChannels == 0 || n == 0 || ! weights.ok)
        return;
    for (auto* d : { &dr, &drLive })
    {
        d->setAmount (pReduction->load() * 0.01f);
        d->setListen (pListen->load() > 0.5f);
    }
    const float outTarget = juce::Decibels::decibelsToGain (pOutput->load());
    const float target = bypass ? 1.0f : 0.0f;
    const int want = pMode->load() > 0.5f ? 1 : 0;
    // in chunks no larger than prepared (a host may exceed its announced block size)
    for (int off = 0; off < n; off += maxChunk)
    {
        const int m = juce::jmin (maxChunk, n - off);
        // STUDIO <-> LIVE: fade out (3 ms), switch once the host compensates the new latency (latency handshake),
        // stay muted until the new path holds valid audio, fade in - no clicks, never misaligned
        float fadeTgt = 1.0f;
        {
            const int mode = activeMode.load();
            const int tgt = hs.isHolding() ? mode : want;
            const bool ready = hs.ready (latencyOf (tgt), fade <= 0.0f, m);
            if (tgt != mode)
            {
                fadeTgt = 0.0f;
                if (fade <= 0.0f && ready)
                {
                    activeMode = tgt;
                    (tgt == 1 ? drLive : dr).reset();
                    latency = latencyOf (tgt);
                    for (auto& d : dryLine)
                        std::fill (d.begin(), d.end(), 0.0f);
                    dryPos = 0;
                    warm = latency + (hs.tracking() ? maxChunk : 0);
                }
            }
            else
                fadeTgt = (! ready || warm > 0) ? 0.0f : 1.0f;
            if (warm > 0)
                warm = juce::jmax (0, warm - m);
        }
        auto& active = activeMode.load() == 1 ? drLive : dr;
        const int D = latency + 1;
        float* io[2] {};
        for (int c = 0; c < nch; ++c)
        {
            io[c] = buffer.getWritePointer (c, off);
            std::copy (io[c], io[c] + m, inCopy[(size_t) c].begin());
        }
        active.process (io, nch, m);
        for (int i = 0; i < m; ++i)
        {
            outGain += (outTarget - outGain) * 0.002f;
            if (std::abs (outGain - outTarget) < 1.0e-4f * std::max (1.0f, outTarget)) // float stall (see Engine.h)
                outGain = outTarget;
            bypassAmt = target > bypassAmt ? juce::jmin (target, bypassAmt + bypassStep) : juce::jmax (target, bypassAmt - bypassStep);
            fade = fadeTgt > fade ? juce::jmin (fadeTgt, fade + fadeStep) : juce::jmax (fadeTgt, fade - fadeStep);
            const int rd = (dryPos + 1) % D; // written exactly `latency` samples ago
            for (int c = 0; c < nch; ++c)
            {
                auto& line = dryLine[(size_t) c];
                line[(size_t) dryPos] = inCopy[(size_t) c][(size_t) i];
                const float dry = line[(size_t) rd];
                float y = io[c][i] * outGain;
                if (bypassAmt >= 1.0f)
                    y = dry;
                else if (bypassAmt > 0.0f)
                    y += (dry - y) * bypassAmt;
                io[c][i] = y * fade;
            }
            dryPos = rd;
        }
    }
}

void DeReverbProcessor::setNonRealtime (bool isOffline) noexcept
{
    AudioProcessor::setNonRealtime (isOffline);
    hs.setNonRealtime (isOffline);
}

// message thread: the latency handshake (a new latency plays only after it was reported to the host)
void DeReverbProcessor::timerCallback()
{
    hs.heartbeat();
    int lat = 0;
    if (preparedChannels > 0 && hs.take (lat))
    {
        if (lat != getLatencySamples())
            setLatencySamples (lat);
        hs.ack();
    }
    // a host that does not compensate in real time (offline render, a test host): the switch happened at once - report
    // the latency that plays now
    else if (preparedChannels > 0 && ! hs.tracking())
    {
        const int now = latencyOf (activeMode.load());
        if (now != getLatencySamples())
            setLatencySamples (now);
    }
}

void DeReverbProcessor::processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) { run (b, false); }
void DeReverbProcessor::processBlockBypassed (juce::AudioBuffer<float>& b, juce::MidiBuffer&) { run (b, true); }

//== presets, A/B ==============================================================================================
const std::vector<DeReverbProcessor::Preset>& DeReverbProcessor::factoryPresets()
{
    static const std::vector<Preset> list {
        { "Natural", "85 %: the best balance measured - most of the room out, the voice intact (the default).", 85.0f },
        { "Clean", "100 %: the voice as the mic would hear it in free space - the driest result.", 100.0f },
        { "Gentle", "60 %: takes the edge off a boxy room; keeps a little air.", 60.0f },
    };
    return list;
}

int DeReverbProcessor::getNumPrograms() { return (int) factoryPresets().size(); }

void DeReverbProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, getNumPrograms() - 1, index);
    currentProgram = index;
    if (auto* p = apvts.getParameter ("reduction"))
        p->setValueNotifyingHost (p->convertTo0to1 (factoryPresets()[(size_t) index].reduction));
    if (auto* p = apvts.getParameter ("listen"))
        p->setValueNotifyingHost (0.0f);
}

const juce::String DeReverbProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, getNumPrograms()) ? juce::String (factoryPresets()[(size_t) index].name) : juce::String();
}

void DeReverbProcessor::selectSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == activeSlot)
        return;
    slots[activeSlot] = apvts.copyState();
    if (slots[slot].isValid())
        restoreTree (slots[slot].createCopy());
    else
        slots[slot] = apvts.copyState();
    activeSlot = slot;
}

void DeReverbProcessor::copyActiveToOther() { slots[1 - activeSlot] = apvts.copyState(); }

// APVTS::replaceState skips a parameter whose snapped value already matches; re-apply explicitly (pluginval scenario)
void DeReverbProcessor::restoreTree (const juce::ValueTree& tree)
{
    apvts.replaceState (tree);
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto child = apvts.state.getChildWithProperty ("id", rp->paramID);
            if (child.isValid() && child.hasProperty ("value"))
            {
                const float norm = rp->convertTo0to1 ((float) child.getProperty ("value"));
                if (std::abs (rp->getValue() - norm) > 1.0e-6f)
                    rp->setValueNotifyingHost (norm);
            }
        }
}

void DeReverbProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void DeReverbProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        restoreTree (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* DeReverbProcessor::createEditor() { return new DeReverbEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new DeReverbProcessor(); }
