#pragma once
// Axell core - the Axell look: colours, Inter fonts, look and feel and the small shared widgets (knob, segmented
// switch, LED / pill toggle, choice, menu button). From Axell Penta EQ 2.0; the visual language of every Axell plugin.
#include <juce_audio_processors/juce_audio_processors.h>

namespace ui
{
juce::Font font (float height, int weight = 0); // 0 regular, 1 semibold, 2 bold
juce::Font caps (float height, int weight = 1);  // tracked caps

namespace col
{
const juce::Colour bg       (0xff121418);
const juce::Colour header   (0xff171a1f);
const juce::Colour panel    (0xff1c1f25);
const juce::Colour panelTop (0xff21252c);
const juce::Colour well     (0xff0d0f12);
const juce::Colour track    (0xff2b2f37);
const juce::Colour grid     (0xff23272e);
const juce::Colour text     (0xffe7e9ee);
const juce::Colour dim      (0xff8d939e);
const juce::Colour faint    (0xff5b616c);
const juce::Colour accent   (0xfff08a3c);   // family orange (Duo's FET colour)
const juce::Colour teal     (0xff33c1b5);   // family teal (Duo's opto colour)
const juce::Colour global   (0xffc3c8d2);
const juce::Colour ok       (0xff58c27d);
const juce::Colour warn     (0xffe6c24a);
const juce::Colour clip     (0xffe5533d);
// character modules
const juce::Colour tube     (0xffd9a55b);   // warm amber
const juce::Colour inductor (0xff6f8fb8);   // steel blue
const juce::Colour console  (0xffb9c2cf);   // console grey
const juce::Colour air      (0xff7fd6e6);   // light cyan
juce::Colour band (int index);              // 0..11 bands, 12 HP, 13 LP
} // namespace col

//==============================================================================
class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float startAngle,
                           float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawLabel (juce::Graphics&, juce::Label&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int w, int h) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
};

//==============================================================================
// Knob = caption + rotary + value box. Can be re-bound to another parameter (band panel).
class Knob : public juce::Component
{
public:
    Knob (const juce::String& caption, juce::Colour accent, int maxDiameter = 56);
    void bind (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& tooltip,
               std::function<void()> onGestureStart = nullptr);
    void unbind();
    void setCaption (const juce::String& c);
    void setAccent (juce::Colour c);
    void resized() override;
    juce::Slider slider;

private:
    juce::Label label;
    int maxD;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// N mutually exclusive buttons bound to a choice/bool parameter (re-bindable)
class Segmented : public juce::Component
{
public:
    Segmented (const juce::StringArray& labels, juce::Colour accent);
    void bind (juce::RangedAudioParameter& p, const juce::String& tooltip, std::function<void()> onGestureStart = nullptr);
    void unbind();
    void resized() override;
    void setAccent (juce::Colour c);

private:
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    std::function<void()> gesture;
};

// LED toggle ("power") or pill
class Toggle : public juce::ToggleButton
{
public:
    Toggle (const juce::String& caption, juce::Colour accent, const juce::String& style);
    void bind (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& tooltip,
               std::function<void()> onGestureStart = nullptr);
    void unbind();
    void mouseDown (const juce::MouseEvent& e) override; // undo snapshot before the change

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    std::function<void()> gestureFn;
};

// combo box bound to a choice parameter (re-bindable)
class Choice : public juce::ComboBox
{
public:
    void bind (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& tooltip,
               std::function<void()> onGestureStart = nullptr);
    void unbind();
    void showPopup() override; // undo snapshot before the change

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    std::function<void()> gestureFn;
};

// a header button with a drop-down chevron
class MenuButton : public juce::TextButton
{
public:
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LevelMeter (juce::String caption);
    void push (float peakL, float peakR, int numChannels);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::String caption;
    float level[2] { -100.0f, -100.0f }, hold[2] { -100.0f, -100.0f }, maxHold = -100.0f;
    int holdFrames[2] {};
    int channels = 2;
};

// helper: draw a hardware-style module panel
void drawModulePanel (juce::Graphics&, juce::Rectangle<float> r, juce::Colour accent, const juce::String& title,
                      const juce::String& subtitle, bool active);
} // namespace ui
