#include "PluginEditor.h"

#include <axell/gui/BrandTitle.h>

#include <cmath>

using namespace ui;

namespace
{
const juce::Identifier kUiWidth ("uiWidth");
namespace tip
{
const char* reduction = "How much of the room is removed: 100 % = the voice as the mic would hear it in free space (the room's "
                        "reflections, its boxy coloration and its tail taken out), 0 % = untouched. 85 % measured the best balance.";
const char* mode = "STUDIO: the full network with 32 ms of look-ahead (53 ms latency, compensated by the DAW) - for "
                   "mixing. LIVE: a causal network applied as a zero-latency filter - for recording, so you hear "
                   "yourself without the room and without delay. LIVE removes a little less of the room.";
const char* listen = "Hear only what is being removed. If you can hear words in it clearly, lower REDUCTION.";
const char* trueTone = "Gives back the low end the room removal takes beyond the room's share, and the slight extra brightness: "
                       "De-Reverb watches what it removes per band and corrects the tone (+-3 dB at most). In rooms like "
                       "yours it halves the tone error against the real voice.";
const char* output = "Output level.";
const char* history = "The last seconds: input (grey), output (white) and the room being removed (teal band, dB re the input).";
const char* removed = "The level of what is being removed (reflections, box, tail), re the input level, smoothed over half a second: -6 dB is a lot of room, -30 dB almost none.";
} // namespace tip
} // namespace

//==============================================================================
void RoomHistory::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto r = getLocalBounds().toFloat();
    g.setColour (col::well);
    g.fillRoundedRectangle (r, 10.0f);
    const auto plot = r.reduced (12.0f, 10.0f).withTrimmedLeft (24.0f).withTrimmedBottom (26.0f);
    const float lo = -72.0f, hi = 0.0f;
    auto yL = [&] (float d) { return plot.getBottom() - (jlimit (lo, hi, d) - lo) / (hi - lo) * plot.getHeight(); };
    g.setFont (ui::font (10.0f));
    for (int k = 1; k < 6; ++k)
    {
        const float d = -12.0f * (float) k;
        g.setColour (col::grid);
        g.drawHorizontalLine ((int) yL (d), plot.getX(), plot.getRight());
        g.setColour (col::faint);
        g.drawText (String ((int) d), Rectangle<float> (r.getX() + 4.0f, yL (d) - 6.0f, 28.0f, 12.0f), Justification::right);
    }
    const int N = (int) ring.size();
    const float w = plot.getWidth() / (float) N;
    for (int i = 0; i < N; ++i)
    {
        const auto& f = ring[(size_t) ((pos + i) % N)];
        const float x = plot.getX() + w * (float) i;
        if (f.in > lo)
        {
            g.setColour (col::dim.withAlpha (0.35f));
            g.fillRect (x, yL (f.in), w * 0.9f, plot.getBottom() - yL (f.in));
        }
        if (f.out > lo)
        {
            g.setColour (col::text.withAlpha (0.55f));
            g.fillRect (x, yL (f.out), w * 0.9f, plot.getBottom() - yL (f.out));
        }
        // the room removed: -40 dB (little) .. 0 dB (as loud as the input) as the band's brightness
        const float a = jlimit (0.0f, 1.0f, (f.removed + 40.0f) / 40.0f);
        g.setColour (col::teal.withAlpha (0.08f + 0.9f * a));
        g.fillRect (x, plot.getBottom() + 8.0f, w * 0.9f, 12.0f);
    }
    g.setFont (ui::caps (9.5f, 1));
    g.setColour (col::faint);
    g.drawText ("ROOM", Rectangle<float> (r.getX() + 2.0f, plot.getBottom() + 7.0f, 32.0f, 14.0f), Justification::left);
    g.drawText ("IN  /  OUT", Rectangle<float> (plot.getRight() - 200.0f, plot.getY(), 196.0f, 12.0f), Justification::right);
}

//==============================================================================
DeReverbEditor::DeReverbEditor (DeReverbProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    const int savedWidth = (int) proc.apvts.state.getProperty (kUiWidth, gui::kWidth);
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);
    reduction.bind (proc.apvts, "reduction", tip::reduction);
    listen.bind (proc.apvts, "listen", tip::listen);
    trueTone.bind (proc.apvts, "trueTone", tip::trueTone);
    if (auto* mp = proc.apvts.getParameter ("mode"))
        mode.bind (*mp, tip::mode);
    output.bind (proc.apvts, "output", tip::output);
    history.setTooltip (tip::history);
    removedTip.setTooltip (tip::removed);
    for (auto* c : std::initializer_list<juce::Component*> { &presetPrev, &presetNext, &presetName, &slotA, &slotB, &slotCopy,
                                                            &reduction, &listen, &trueTone, &mode, &output, &inMeter, &outMeter, &history, &removedTip })
        content.addAndMakeVisible (c);
    for (auto* b : { &slotA, &slotB })
        b->setColour (juce::TextButton::buttonOnColourId, col::global);
    presetPrev.onClick = [this] { proc.setCurrentProgram ((proc.getCurrentProgram() + proc.getNumPrograms() - 1) % proc.getNumPrograms()); };
    presetNext.onClick = [this] { proc.setCurrentProgram ((proc.getCurrentProgram() + 1) % proc.getNumPrograms()); };
    presetName.onClick = [this] { showPresetMenu(); };
    slotA.onClick = [this] { proc.selectSlot (0); };
    slotB.onClick = [this] { proc.selectSlot (1); };
    slotCopy.onClick = [this] { proc.copyActiveToOther(); };
    presetName.setTooltip ("Presets.");
    slotA.setTooltip ("A/B: two complete settings to compare.");
    slotB.setTooltip ("A/B: two complete settings to compare.");
    slotCopy.setTooltip ("Copy the current slot to the other one.");
    setResizable (true, true);
    setResizeLimits (gui::kWidth * 3 / 4, gui::kHeight * 3 / 4, gui::kWidth * 2, gui::kHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) gui::kWidth / gui::kHeight);
    const int w = juce::jlimit (gui::kWidth * 3 / 4, gui::kWidth * 2, savedWidth);
    setSize (w, juce::roundToInt (w * (double) gui::kHeight / gui::kWidth));
    constructed = true;
    timerCallback();
    startTimerHz (30);
}

DeReverbEditor::~DeReverbEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void DeReverbEditor::paint (juce::Graphics& g) { g.fillAll (col::bg); }

void DeReverbEditor::resized()
{
    content.setBounds (0, 0, gui::kWidth, gui::kHeight);
    content.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) gui::kWidth));
    if (constructed)
        proc.apvts.state.setProperty (kUiWidth, getWidth(), nullptr);
}

void DeReverbEditor::Content::resized() { owner.layout(); }

void DeReverbEditor::layout()
{
    using juce::Rectangle;
    const int m = 16;
    headerArea = { 0, 0, gui::kWidth, 58 };
    {
        auto pb = Rectangle<int> (330, 14, 330, 30);
        presetPrev.setBounds (pb.removeFromLeft (30));
        pb.removeFromLeft (4);
        presetNext.setBounds (pb.removeFromRight (30));
        pb.removeFromRight (4);
        presetName.setBounds (pb);
        auto ab = Rectangle<int> (gui::kWidth - m - 160, 14, 160, 30);
        slotA.setBounds (ab.removeFromLeft (34));
        ab.removeFromLeft (4);
        slotB.setBounds (ab.removeFromLeft (34));
        ab.removeFromLeft (8);
        slotCopy.setBounds (ab);
    }
    history.setBounds (m, 72, gui::kWidth - 2 * m, 230);
    controlArea = { m, 314, gui::kWidth - 2 * m, gui::kHeight - 314 - m };
    auto a = controlArea.reduced (18, 14);
    outMeter.setBounds (a.removeFromRight (44));
    a.removeFromRight (6);
    inMeter.setBounds (a.removeFromRight (44));
    a.removeFromRight (20);
    output.setBounds (a.removeFromRight (110));
    a.removeFromRight (20);
    reduction.setBounds (a.removeFromLeft (230));
    a.removeFromLeft (24);
    auto mid = a;
    removedArea = mid.removeFromTop (110).reduced (0, 6);
    removedTip.setBounds (removedArea);
    mid.removeFromTop (12);
    {
        auto row = mid.removeFromTop (40).withSizeKeepingCentre (260, 36);
        listen.setBounds (row.removeFromLeft (152));
        row.removeFromLeft (8);
        trueTone.setBounds (row);
    }
    mid.removeFromTop (6);
    mode.setBounds (mid.removeFromTop (32).withSizeKeepingCentre (260, 30));
}

void DeReverbEditor::Content::paint (juce::Graphics& g)
{
    using namespace juce;
    auto& e = owner;
    g.fillAll (col::bg);
    g.setColour (col::header);
    g.fillRect (e.headerArea);
    axell::brand::drawHeaderRule (g, (float) e.headerArea.getBottom() - 1.0f, 0.0f, (float) gui::kWidth, col::grid);
    // Axell's Dojo: the family emblem, its wave and dot in this plugin's colour
    axell::brand::drawEmblem (g, { 12.0f, 6.0f, 46.0f, 46.0f }, juce::Colour (0xff4fb3d9));
    axell::brand::drawVersion (g, axell::brand::drawTitle (g, 68.0f, 28.0f, "De-Reverb",
                                                               juce::String (juce::CharPointer_UTF8 ("\xe9\x9d\x99"))), // 静
                               28.0f, AXELL_VERSION_STRING);
    g.setFont (ui::caps (10.0f, 1));
    g.setColour (col::dim);
    g.drawText ("ROOM REMOVAL FOR VOCALS", 67, 37, 320, 14, Justification::centredLeft);
    const auto r = e.controlArea.toFloat();
    g.setGradientFill (ColourGradient (col::panelTop, 0, r.getY(), col::panel, 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (col::teal.withAlpha (0.4f));
    g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.2f);
    // the room-removed meter
    const auto ra = e.removedArea;
    g.setColour (col::well);
    g.fillRoundedRectangle (ra.toFloat(), 8.0f);
    auto in = ra.reduced (14, 10);
    g.setColour (col::dim);
    g.setFont (ui::caps (10.5f, 1));
    g.drawText ("REMOVED, RE INPUT", in.removeFromTop (16), Justification::centredLeft);
    const float v = e.removedShown;
    g.setFont (ui::font (30.0f, 2));
    g.setColour (v > -119.0f ? col::text : col::faint);
    g.drawText (v > -119.0f ? String (v, 1) + " dB" : String ("-"), in.removeFromTop (40), Justification::centredLeft);
    const auto bar = in.removeFromTop (14).toFloat();
    g.setColour (col::track);
    g.fillRoundedRectangle (bar, 4.0f);
    g.setColour (col::teal);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * jlimit (0.0f, 1.0f, (v + 40.0f) / 40.0f)), 4.0f);
    g.setFont (ui::font (11.0f));
    g.setColour (col::faint);
    const double fs = 48000.0;
    juce::ignoreUnused (fs);
    g.drawText ("latency " + String (e.proc.getLatencySamples()) + " samples (compensated)",
                e.controlArea.reduced (18, 8).removeFromBottom (16), Justification::centredLeft);
    if (! e.proc.weightsOk())
    {
        g.setColour (col::clip);
        g.drawText ("Network weights missing or damaged - the plugin passes audio through.", e.controlArea.reduced (18, 8).removeFromBottom (32).removeFromTop (16),
                    Justification::centredLeft);
    }
}

void DeReverbEditor::timerCallback()
{
    const auto& core = proc.dsp().core();
    RoomHistory::Frame f { core.inputDb(), core.outputDb(), core.removedDb() };
    history.push (f);
    auto lin = [] (float db) { return db > -119.0f ? std::pow (10.0f, db / 20.0f) : 0.0f; };
    inMeter.push (lin (f.in), lin (f.in), 2);
    outMeter.push (lin (f.out), lin (f.out), 2);
    removedShown = f.in > -80.0f ? f.removed : -120.0f;
    presetName.setButtonText (proc.getProgramName (proc.getCurrentProgram()));
    slotA.setToggleState (proc.getActiveSlot() == 0, juce::dontSendNotification);
    slotB.setToggleState (proc.getActiveSlot() == 1, juce::dontSendNotification);
    slotCopy.setButtonText (proc.getActiveSlot() == 0 ? "COPY A>B" : "COPY B>A");
    content.repaint (controlArea);
}

void DeReverbEditor::showPresetMenu()
{
    juce::PopupMenu menu;
    const auto& list = DeReverbProcessor::factoryPresets();
    for (int i = 0; i < (int) list.size(); ++i)
        menu.addItem (1 + i, list[(size_t) i].name);
    juce::Component::SafePointer<DeReverbEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetName), [safe] (int id) {
        if (safe != nullptr && id > 0)
            safe->proc.setCurrentProgram (id - 1);
    });
}
