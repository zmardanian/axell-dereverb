#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "DeReverb.h"

// Thin JUCE wrapper around axdr::DeReverb (all DSP lives in JUCE-free headers). The network's weights are embedded
// (BinaryData) and verified on load.
class DeReverbProcessor : public juce::AudioProcessor
{
public:
    DeReverbProcessor();
    ~DeReverbProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    struct Preset { const char* name; const char* description; float reduction; };
    static const std::vector<Preset>& factoryPresets();
    int getActiveSlot() const { return activeSlot; }
    void selectSlot (int slot);
    void copyActiveToOther();
    bool weightsOk() const { return weights.ok; }
    const axdr::DeReverb& dsp() const { return dr; }

private:
    void run (juce::AudioBuffer<float>& buffer, bool bypass);
    void restoreTree (const juce::ValueTree& tree);

    axdr::Weights weights;
    axdr::DeReverb dr;
    int preparedChannels = 0, latency = 0;
    // host bypass: the input delayed by the latency, crossfaded in over 10 ms
    std::array<std::vector<float>, 2> dryLine, inCopy;
    int maxChunk = 512;
    int dryPos = 0;
    float bypassAmt = 0.0f, bypassStep = 0.001f;
    int currentProgram = 0, activeSlot = 0;
    juce::ValueTree slots[2];
    std::atomic<float>* pReduction = nullptr;
    std::atomic<float>* pListen = nullptr;
    std::atomic<float>* pOutput = nullptr;
    float outGain = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeReverbProcessor)
};
