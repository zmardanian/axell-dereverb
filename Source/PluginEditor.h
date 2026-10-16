#pragma once
#include <axell/gui/LookAndFeel.h>
#include "PluginProcessor.h"

namespace gui
{
constexpr int kWidth = 980;
constexpr int kHeight = 560;
}

// a transparent area that only shows a tooltip (over painted displays)
struct TipArea : public juce::Component, public juce::SettableTooltipClient
{
    TipArea() { setInterceptsMouseClicks (true, false); }
};

// the last ~8 s: input and output level, and the room removed (re the input) as a band along the bottom
class RoomHistory : public juce::Component, public juce::SettableTooltipClient
{
public:
    RoomHistory() : ring (240) {}
    struct Frame { float in = -120.0f, out = -120.0f, removed = -120.0f; };
    void push (Frame f) { ring[(size_t) pos] = f; pos = (pos + 1) % (int) ring.size(); repaint(); }
    void paint (juce::Graphics&) override;

private:
    std::vector<Frame> ring;
    int pos = 0;
};

class DeReverbEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DeReverbEditor (DeReverbProcessor&);
    ~DeReverbEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Content : public juce::Component
    {
    public:
        explicit Content (DeReverbEditor& e) : owner (e) {}
        void paint (juce::Graphics&) override;
        void resized() override;
        DeReverbEditor& owner;
    };
    void timerCallback() override;
    void layout();
    void showPresetMenu();

    DeReverbProcessor& proc;
    ui::LookAndFeel lnf;
    Content content { *this };
    juce::TooltipWindow tooltips { this, 700 };
    juce::TextButton presetPrev { "<" }, presetNext { ">" };
    ui::MenuButton presetName;
    juce::TextButton slotA { "A" }, slotB { "B" }, slotCopy { "COPY" };
    ui::Knob reduction { "REDUCTION", ui::col::teal, 150 }, output { "OUTPUT", ui::col::global, 72 };
    ui::Toggle listen { "LISTEN: REMOVED", ui::col::warn, "pill" };
    ui::LevelMeter inMeter { "IN" }, outMeter { "OUT" };
    RoomHistory history;
    TipArea removedTip;
    juce::Rectangle<int> headerArea, controlArea, removedArea;
    float removedShown = -120.0f;
    bool constructed = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeReverbEditor)
};
