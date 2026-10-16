#include "axell/gui/LookAndFeel.h"
#include "AxellCoreFonts.h"

namespace ui
{
namespace
{
juce::Typeface::Ptr typeface (int weight)
{
    static juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (AxellFonts::InterRegular_otf,
                                                                                    AxellFonts::InterRegular_otfSize);
    static juce::Typeface::Ptr semi = juce::Typeface::createSystemTypefaceFor (AxellFonts::InterSemiBold_otf,
                                                                                 AxellFonts::InterSemiBold_otfSize);
    static juce::Typeface::Ptr bold = juce::Typeface::createSystemTypefaceFor (AxellFonts::InterBold_otf,
                                                                                 AxellFonts::InterBold_otfSize);
    return weight >= 2 ? bold : (weight == 1 ? semi : regular);
}
} // namespace

juce::Font font (float height, int weight)
{
    return juce::Font (juce::FontOptions (typeface (weight)).withHeight (height));
}

juce::Font caps (float h, int weight)
{
    auto f = font (h, weight);
    f.setExtraKerningFactor (0.08f);
    return f;
}

juce::Colour col::band (int i)
{
    static const juce::uint32 c[] = { 0xfff08a3c, 0xffe6c24a, 0xff9ccc65, 0xff4fc3a1, 0xff33c1b5, 0xff4fa3e0,
                                      0xff7986cb, 0xffb388eb, 0xffe57fbf, 0xffef6f6c, 0xffd4a373, 0xff9fb3c8,
                                      0xffc3c8d2, 0xffc3c8d2 };
    return juce::Colour (c[juce::jlimit (0, 13, i)]);
}

//==============================================================================
LookAndFeel::LookAndFeel()
{
    using namespace juce;
    setColour (ResizableWindow::backgroundColourId, col::bg);
    setColour (PopupMenu::backgroundColourId, Colour (0xff1e2128));
    setColour (PopupMenu::textColourId, col::text);
    setColour (PopupMenu::headerTextColourId, col::dim);
    setColour (PopupMenu::highlightedBackgroundColourId, Colour (0xff343944));
    setColour (PopupMenu::highlightedTextColourId, col::text);
    setColour (Slider::textBoxTextColourId, col::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, col::well);
    setColour (Slider::textBoxHighlightColourId, col::accent.withAlpha (0.4f));
    setColour (Label::textColourId, col::text);
    setColour (TextEditor::backgroundColourId, col::well);
    setColour (TextEditor::textColourId, col::text);
    setColour (TextEditor::highlightColourId, col::accent.withAlpha (0.4f));
    setColour (TextEditor::outlineColourId, col::track);
    setColour (TextEditor::focusedOutlineColourId, col::accent);
    setColour (CaretComponent::caretColourId, col::text);
    setColour (TooltipWindow::backgroundColourId, Colour (0xf0262a32));
    setColour (TooltipWindow::textColourId, col::text);
    setColour (TooltipWindow::outlineColourId, col::track);
    setColour (AlertWindow::backgroundColourId, Colour (0xff1e2128));
    setColour (AlertWindow::textColourId, col::text);
    setColour (AlertWindow::outlineColourId, col::track);
    setColour (TextButton::buttonColourId, col::track);
    setColour (TextButton::textColourOffId, col::text);
    setColour (ComboBox::backgroundColourId, col::well);
    setColour (ComboBox::textColourId, col::text);
    setColour (ComboBox::outlineColourId, col::track);
    setColour (ComboBox::arrowColourId, col::dim);
    setColour (ListBox::backgroundColourId, col::well);
    setColour (ScrollBar::thumbColourId, col::track.brighter (0.2f));
}

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    const auto style = f.getTypefaceStyle();
    if (f.isBold() || style.containsIgnoreCase ("bold"))
        return typeface (2);
    if (style.containsIgnoreCase ("semi") || style.containsIgnoreCase ("medium"))
        return typeface (1);
    return typeface (0);
}

void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float startAngle,
                                    float endAngle, juce::Slider& s)
{
    using namespace juce;
    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
    const float size = jmin (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const float r = size * 0.5f;
    const float track = jmax (2.5f, r * 0.09f);
    const float angle = startAngle + pos * (endAngle - startAngle);
    const auto accent = s.findColour (Slider::rotarySliderFillColourId);
    const bool enabled = s.isEnabled();
    const PathStrokeType stroke (track, PathStrokeType::curved, PathStrokeType::rounded);
    const float arcR = r - track * 0.5f;

    Path bg;
    bg.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (col::track);
    g.strokePath (bg, stroke);

    // stepped selectors draw their detents
    const auto range = s.getRange();
    if (s.getInterval() >= 1.0 && range.getLength() <= 12.0 && range.getLength() >= 1.0)
    {
        const int steps = (int) range.getLength();
        for (int i = 0; i <= steps; ++i)
        {
            const float a = startAngle + (float) i / (float) steps * (endAngle - startAngle);
            const auto p = centre.getPointOnCircumference (r + 1.5f, a);
            g.setColour (col::faint);
            g.fillEllipse (p.x - 1.2f, p.y - 1.2f, 2.4f, 2.4f);
        }
    }

    float from = startAngle;
    if (range.getStart() < 0.0 && range.getEnd() > 0.0)
        from = startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle);
    Path val;
    val.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, jmin (from, angle), jmax (from, angle), true);
    g.setColour (enabled ? accent : accent.withSaturation (0.1f).withAlpha (0.5f));
    g.strokePath (val, stroke);

    const auto body = Rectangle<float> (size, size).withCentre (centre).reduced (track * 2.4f);
    g.setColour (Colours::black.withAlpha (0.45f));
    g.fillEllipse (body.translated (0.0f, size * 0.025f).expanded (1.0f));
    g.setGradientFill (ColourGradient (Colour (0xff3d424c), body.getX(), body.getY(), Colour (0xff1d2026),
                                       body.getRight(), body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (Colour (0xff4a505b));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    const float pr = body.getWidth() * 0.5f;
    Path pointer;
    const float pw = jmax (2.0f, pr * 0.1f);
    pointer.addRoundedRectangle (-pw * 0.5f, -pr + pr * 0.14f, pw, pr * 0.42f, pw * 0.5f);
    g.setColour (enabled ? accent.brighter (0.25f) : col::dim);
    g.fillPath (pointer, AffineTransform::rotation (angle).translated (centre.x, centre.y));
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    using namespace juce;
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const auto accent = b.findColour (TextButton::buttonOnColourId);
    const bool on = b.getToggleState();
    const float corner = 5.0f;
    if (on)
    {
        g.setColour (accent.withAlpha (b.isEnabled() ? 0.95f : 0.4f));
        g.fillRoundedRectangle (r, corner);
    }
    else
    {
        g.setColour (col::well.brighter (over ? 0.25f : 0.08f).withMultipliedAlpha (down ? 0.8f : 1.0f));
        g.fillRoundedRectangle (r, corner);
        g.setColour (col::track.brighter (over ? 0.3f : 0.0f));
        g.drawRoundedRectangle (r, corner, 1.0f);
    }
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return caps (juce::jmin (12.5f, (float) buttonHeight * 0.48f), 1);
}

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    const bool on = b.getToggleState();
    g.setColour (on ? juce::Colour (0xff121418) : (b.isEnabled() ? col::text : col::faint));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (3, 0), juce::Justification::centred, 1, 0.85f);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    using namespace juce;
    const auto accent = b.findColour (ToggleButton::tickColourId);
    const bool on = b.getToggleState();
    const auto style = b.getProperties()["style"].toString();
    auto r = b.getLocalBounds().toFloat();
    const float alpha = b.isEnabled() ? 1.0f : 0.4f;

    if (style == "pill")
    {
        r = r.reduced (0.5f);
        g.setColour ((on ? accent.withAlpha (0.18f) : col::well.brighter (over ? 0.2f : 0.06f)).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour ((on ? accent : col::track.brighter (over ? 0.3f : 0.0f)).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.2f);
        const float d = r.getHeight() * 0.34f;
        auto led = Rectangle<float> (d, d).withCentre ({ r.getX() + r.getHeight() * 0.55f, r.getCentreY() });
        g.setColour ((on ? accent : col::faint).withMultipliedAlpha (alpha));
        g.fillEllipse (led);
        g.setColour ((on ? col::text : col::dim).withMultipliedAlpha (alpha));
        g.setFont (caps (jmin (11.5f, r.getHeight() * 0.48f), 1));
        g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (r.getHeight()).withTrimmedRight (r.getHeight() * 0.3f).toNearestInt(),
                          Justification::centred, 1, 0.85f);
        return;
    }

    const float d = jmin (r.getHeight() * 0.5f, 12.0f);
    auto led = Rectangle<float> (d, d).withCentre ({ r.getX() + d * 0.8f, r.getCentreY() });
    if (on)
    {
        g.setColour (accent.withAlpha (0.25f * alpha));
        g.fillEllipse (led.expanded (d * 0.35f));
    }
    g.setColour ((on ? accent : col::faint).withMultipliedAlpha (alpha));
    g.fillEllipse (led);
    g.setColour (Colours::black.withAlpha (0.4f));
    g.drawEllipse (led, 1.0f);
    g.setColour ((on ? col::text : col::dim).withMultipliedAlpha (alpha));
    g.setFont (caps (jmin (13.0f, r.getHeight() * 0.58f), 2));
    g.drawText (b.getButtonText(), r.withTrimmedLeft (d * 1.8f), Justification::centredLeft);
}

juce::Font LookAndFeel::getLabelFont (juce::Label& l)
{
    return font (juce::jmin (13.0f, (float) l.getHeight() * 0.72f), 0);
}

void LookAndFeel::drawLabel (juce::Graphics& g, juce::Label& l)
{
    using namespace juce;
    auto* parentSlider = dynamic_cast<Slider*> (l.getParentComponent());
    const bool inSlider = parentSlider != nullptr;
    const bool bar = inSlider && parentSlider->getSliderStyle() == Slider::LinearBar; // drawn by drawLinearSlider
    if (inSlider && ! bar)
    {
        g.setColour (col::well);
        g.fillRoundedRectangle (l.getLocalBounds().toFloat(), 4.0f);
    }
    else if (! inSlider)
        g.fillAll (l.findColour (Label::backgroundColourId));
    if (! l.isBeingEdited())
    {
        g.setColour (l.findColour (Label::textColourId).withMultipliedAlpha (l.isEnabled() ? 1.0f : 0.5f));
        g.setFont (inSlider ? font (jmin (12.0f, (float) l.getHeight() * 0.7f), 1) : l.getFont());
        g.drawFittedText (l.getText(), l.getLocalBounds().reduced (2, 0), l.getJustificationType(), 1, 0.8f);
    }
}

// bar sliders (Gain Scale, Match): a well, the value as a fill, an outline; the text is the label on top
void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                                    juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearBar)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, minPos, maxPos, style, s);
        return;
    }
    const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (0.5f);
    g.setColour (col::well);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (s.findColour (juce::Slider::trackColourId));
    g.fillRoundedRectangle (r.withRight (juce::jlimit (r.getX(), r.getRight(), pos)), 4.0f);
    g.setColour (col::track);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

juce::Font LookAndFeel::getPopupMenuFont() { return font (14.5f, 0); }
juce::Font LookAndFeel::getAlertWindowMessageFont() { return font (14.5f, 0); }
juce::Font LookAndFeel::getAlertWindowTitleFont() { return font (17.0f, 2); }
juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox& b) { return font (juce::jmin (13.0f, (float) b.getHeight() * 0.5f), 1); }

void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& b)
{
    using namespace juce;
    const auto r = Rectangle<float> ((float) w, (float) h).reduced (0.5f);
    g.setColour (col::well.brighter (b.isMouseOver (true) ? 0.2f : 0.06f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (b.findColour (ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
    Path p;
    const float cx = (float) w - 11.0f, cy = (float) h * 0.5f;
    p.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour (col::dim.withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.4f));
    g.fillPath (p);
}

void LookAndFeel::positionComboBoxText (juce::ComboBox& b, juce::Label& l)
{
    l.setBounds (6, 1, b.getWidth() - 24, b.getHeight() - 2);
    l.setFont (getComboBoxFont (b));
}

void LookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    using namespace juce;
    const auto r = Rectangle<float> ((float) w, (float) h);
    g.setColour (findColour (TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (findColour (TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    AttributedString s;
    s.setJustification (Justification::topLeft);
    s.append (text, font (13.5f), findColour (TooltipWindow::textColourId));
    TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, (float) w - 20.0f);
    tl.draw (g, r.reduced (10.0f, 8.0f));
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                    juce::Rectangle<int> parentArea)
{
    using namespace juce;
    AttributedString s;
    s.append (tipText, font (13.5f));
    TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, 320.0f);
    const int w = (int) std::ceil (tl.getWidth()) + 22, h = (int) std::ceil (tl.getHeight()) + 18;
    return Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                           screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
        .constrainedWithin (parentArea);
}

void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (col::well);
    g.fillRoundedRectangle (juce::Rectangle<float> ((float) w, (float) h), 5.0f);
}

void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.hasKeyboardFocus (true) ? col::accent : col::track);
    g.drawRoundedRectangle (juce::Rectangle<float> ((float) w, (float) h).reduced (0.5f), 5.0f, 1.0f);
}

//==============================================================================
Knob::Knob (const juce::String& caption, juce::Colour accent, int maxDiameter) : maxD (maxDiameter)
{
    using namespace juce;
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setRotaryParameters (MathConstants<float>::pi * 1.22f, MathConstants<float>::pi * 2.78f, true);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, 72, 18);
    slider.setColour (Slider::rotarySliderFillColourId, accent);
    slider.setMouseDragSensitivity (220);
    slider.setVelocityModeParameters (0.35, 1, 0.0, true,
                                      (ModifierKeys::Flags) (ModifierKeys::ctrlModifier | ModifierKeys::shiftModifier));
    addAndMakeVisible (slider);
    label.setText (caption, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (caps (11.0f, 1));
    label.setColour (Label::textColourId, col::dim);
    label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (label);
}

void Knob::bind (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& tooltip,
                 std::function<void()> onGestureStart)
{
    attachment.reset();
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramId, slider);
    if (auto* p = apvts.getParameter (paramId))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    slider.setTooltip (tooltip);
    slider.onDragStart = std::move (onGestureStart);
}

void Knob::unbind()
{
    attachment.reset();
}

void Knob::setCaption (const juce::String& c) { label.setText (c, juce::dontSendNotification); }
void Knob::setAccent (juce::Colour c) { slider.setColour (juce::Slider::rotarySliderFillColourId, c); slider.repaint(); }

void Knob::resized()
{
    constexpr int labelH = 15, boxH = 18;
    auto b = getLocalBounds();
    const int d = juce::jmax (16, juce::jmin (maxD, b.getWidth() - 4, b.getHeight() - labelH - boxH - 2));
    const int total = labelH + 1 + d + boxH;
    b = b.withSizeKeepingCentre (b.getWidth(), total);
    label.setBounds (b.removeFromTop (labelH));
    b.removeFromTop (1);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jmin (b.getWidth(), 76), boxH);
    slider.setBounds (b.withSizeKeepingCentre (juce::jmax (d, juce::jmin (b.getWidth(), 76)), d + boxH));
}

//==============================================================================
Segmented::Segmented (const juce::StringArray& labels, juce::Colour accent)
{
    for (int i = 0; i < labels.size(); ++i)
    {
        auto* b = buttons.add (new juce::TextButton (labels[i]));
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, accent);
        b->onClick = [this, i]
        {
            if (gesture)
                gesture();
            if (attachment)
                attachment->setValueAsCompleteGesture ((float) i);
        };
        addAndMakeVisible (b);
    }
}

void Segmented::bind (juce::RangedAudioParameter& p, const juce::String& tooltip, std::function<void()> onGestureStart)
{
    gesture = std::move (onGestureStart);
    for (auto* b : buttons)
        b->setTooltip (tooltip);
    attachment = std::make_unique<juce::ParameterAttachment> (p, [this] (float v)
    {
        const int idx = juce::roundToInt (v);
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == idx, juce::dontSendNotification);
    });
    attachment->sendInitialUpdate();
}

void Segmented::unbind() { attachment.reset(); }

void Segmented::setAccent (juce::Colour c)
{
    for (auto* b : buttons)
        b->setColour (juce::TextButton::buttonOnColourId, c);
}

void Segmented::resized()
{
    auto b = getLocalBounds();
    const int gap = 3, n = buttons.size();
    const int w = (b.getWidth() - gap * (n - 1)) / juce::jmax (1, n);
    for (int i = 0; i < n; ++i)
    {
        buttons[i]->setBounds (i == n - 1 ? b : b.removeFromLeft (w));
        b.removeFromLeft (gap);
    }
}

//==============================================================================
Toggle::Toggle (const juce::String& caption, juce::Colour accent, const juce::String& style) : juce::ToggleButton (caption)
{
    setColour (juce::ToggleButton::tickColourId, accent);
    getProperties().set ("style", style);
}

void Toggle::bind (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& tooltip,
                   std::function<void()> onGestureStart)
{
    attachment.reset();
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, paramId, *this);
    setTooltip (tooltip);
    gestureFn = std::move (onGestureStart);
}

void Toggle::unbind() { attachment.reset(); }

void Toggle::mouseDown (const juce::MouseEvent& e)
{
    if (gestureFn)
        gestureFn();
    juce::ToggleButton::mouseDown (e);
}

//==============================================================================
void Choice::bind (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId, const juce::String& tooltip,
                   std::function<void()> onGestureStart)
{
    attachment.reset();
    clear (juce::dontSendNotification);
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (paramId)))
        addItemList (p->choices, 1);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, paramId, *this);
    setTooltip (tooltip);
    gestureFn = std::move (onGestureStart);
}

void Choice::unbind() { attachment.reset(); }

void Choice::showPopup()
{
    if (gestureFn)
        gestureFn();
    juce::ComboBox::showPopup();
}

//==============================================================================
void MenuButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    getLookAndFeel().drawButtonBackground (g, *this, findColour (juce::TextButton::buttonColourId), over, down);
    g.setColour (col::text);
    g.setFont (font (14.0f, 1));
    auto r = getLocalBounds().reduced (12, 0);
    auto arrowArea = r.removeFromRight (12).toFloat();
    g.drawFittedText (getButtonText(), r, juce::Justification::centred, 1, 0.8f);
    juce::Path p;
    const auto c = arrowArea.getCentre();
    p.addTriangle (c.x - 4.0f, c.y - 2.0f, c.x + 4.0f, c.y - 2.0f, c.x, c.y + 3.0f);
    g.setColour (col::dim);
    g.fillPath (p);
}

//==============================================================================
LevelMeter::LevelMeter (juce::String c) : caption (std::move (c)) {}

void LevelMeter::push (float l, float r, int numChannels)
{
    channels = juce::jlimit (1, 2, numChannels);
    const float in[2] { juce::Decibels::gainToDecibels (l, -100.0f), juce::Decibels::gainToDecibels (r, -100.0f) };
    for (int c = 0; c < 2; ++c)
    {
        level[c] = in[c] > level[c] ? in[c] : juce::jmax (in[c], level[c] - 0.8f);
        if (in[c] >= hold[c])
        {
            hold[c] = in[c];
            holdFrames[c] = 45;
        }
        else if (--holdFrames[c] < 0)
            hold[c] = juce::jmax (level[c], hold[c] - 0.8f);
        maxHold = juce::jmax (maxHold, in[c]);
    }
    repaint();
}

void LevelMeter::mouseDown (const juce::MouseEvent&)
{
    maxHold = -100.0f;
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    using namespace juce;
    auto b = getLocalBounds().toFloat();
    g.setColour (col::dim);
    g.setFont (caps (10.0f, 1));
    g.drawText (caption, b.removeFromTop (14.0f), Justification::centred);
    auto readout = b.removeFromBottom (16.0f);
    b.removeFromBottom (2.0f);
    g.setColour (col::well);
    g.fillRoundedRectangle (b, 3.0f);
    constexpr float lo = -48.0f, hi = 6.0f;
    auto yFor = [&] (float db) { return b.getBottom() - b.getHeight() * (jlimit (lo, hi, db) - lo) / (hi - lo); };
    const auto bars = b.reduced (2.0f);
    const float gap = 2.0f;
    const float bw = (bars.getWidth() - gap * (float) (channels - 1)) / (float) channels;
    for (int c = 0; c < channels; ++c)
    {
        const auto bar = Rectangle<float> (bars.getX() + (float) c * (bw + gap), bars.getY(), bw, bars.getHeight());
        const float top = yFor (level[c]);
        if (top < bar.getBottom())
        {
            ColourGradient grad (col::clip, 0.0f, yFor (0.0f), col::ok, 0.0f, yFor (-18.0f), false);
            grad.addColour (0.35, col::warn);
            g.setGradientFill (grad);
            g.fillRect (Rectangle<float>::leftTopRightBottom (bar.getX(), top, bar.getRight(), bar.getBottom()));
        }
        if (hold[c] > lo)
        {
            g.setColour (hold[c] > 0.0f ? col::clip : col::text.withAlpha (0.85f));
            g.fillRect (bar.getX(), yFor (hold[c]) - 1.0f, bar.getWidth(), 2.0f);
        }
    }
    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f })
    {
        g.setColour (db > -0.5f ? col::clip.withAlpha (0.5f) : Colour (0x30ffffff));
        g.drawHorizontalLine ((int) yFor (db), b.getX(), b.getRight());
    }
    g.setColour (maxHold > 0.0f ? col::clip : col::text);
    g.setFont (font (10.5f, 1));
    g.drawText (maxHold <= -99.0f ? String ("-inf") : String (maxHold, 1), readout, Justification::centred);
}

//==============================================================================
void drawModulePanel (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent, const juce::String& title,
                      const juce::String& subtitle, bool active)
{
    using namespace juce;
    // brushed-metal style face, darker when off
    g.setGradientFill (ColourGradient (col::panelTop.brighter (0.05f), 0.0f, r.getY(), col::panel.darker (0.15f), 0.0f,
                                       r.getBottom(), false));
    g.fillRoundedRectangle (r, 8.0f);
    for (float y = r.getY() + 3.0f; y < r.getBottom() - 3.0f; y += 3.0f)
    {
        g.setColour (Colours::white.withAlpha (0.012f));
        g.drawHorizontalLine ((int) y, r.getX() + 4.0f, r.getRight() - 4.0f);
    }
    g.setColour (accent.withAlpha (active ? 0.6f : 0.18f));
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.2f);
    // title plate
    auto plate = r.withHeight (24.0f).reduced (8.0f, 4.0f);
    g.setColour (accent.withAlpha (active ? 0.9f : 0.35f));
    g.fillRoundedRectangle (plate.getX(), plate.getBottom() + 1.0f, 22.0f, 2.0f, 1.0f);
    g.setColour (active ? col::text : col::dim);
    g.setFont (caps (11.5f, 2));
    g.drawText (title, plate.withTrimmedLeft (18.0f), Justification::centredLeft);
    if (subtitle.isNotEmpty())
    {
        g.setColour (col::faint);
        g.setFont (caps (9.0f, 1));
        g.drawText (subtitle, plate, Justification::centredRight);
    }
    // screws
    for (auto p : { Point<float> (r.getX() + 7.0f, r.getBottom() - 7.0f), Point<float> (r.getRight() - 7.0f, r.getBottom() - 7.0f) })
    {
        g.setColour (Colour (0xff2d3139));
        g.fillEllipse (p.x - 2.5f, p.y - 2.5f, 5.0f, 5.0f);
        g.setColour (Colour (0xff15171b));
        g.drawLine (p.x - 1.6f, p.y, p.x + 1.6f, p.y, 1.0f);
    }
}
} // namespace ui
